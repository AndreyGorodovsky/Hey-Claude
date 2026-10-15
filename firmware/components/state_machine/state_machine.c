/*
 * The state machine. See state_machine.h for its place in the firmware.
 *
 * How it is built. Everything that can happen arrives as a small message in
 * one queue (s_events), and one task, created here, takes the messages out
 * one at a time and acts on each. Because only that task decides and only
 * it changes the state, no two decisions can ever overlap, and the
 * variables it works with need no lock. It never waits on the network:
 * sending is the upload task's job.
 *
 * Most messages are put in the queue directly by the part that has
 * something to report: the link, the player and the upload each call a
 * function here, from their own tasks. Wake-word events are the exception.
 * They are announced on the default event loop, as they were before this
 * component existed and as the `wake` console command also expects, so a
 * handler registered here copies them into the queue.
 *
 * Time limits use the queue too. The task waits for the next message only
 * until the current deadline, if there is one, and when a wait runs out it
 * looks at the clock to see whether the deadline has passed. So a deadline
 * needs no timer and cannot fire in the middle of another decision. (A
 * wait also runs out once a second with no deadline due, so that the task
 * can check in with the task watchdog; see state_machine_task.)
 *
 * Phases. The device state that the screen shows (app_state.h) has eight
 * values, chosen for what a person needs to see. The state machine needs
 * finer steps than that, so it keeps its own: the phase. Each phase is
 * shown as one device state, and several phases can share one.
 *
 *   Phase        Shown as     Meaning, and what ends it
 *   CONNECTING   CONNECTING   No server connection; the screen says why
 *                             the last attempt failed. SERVER_LINK_UP: to IDLE.
 *   IDLE         IDLE         Waiting for the wake word. A detection
 *                             plays the chime: to CHIME.
 *   CHIME          CAPTURING    The chime is playing; nothing is
 *                             sent yet, so the sound is not in the request.
 *                             Sound over: to CAPTURING.
 *   CAPTURING    CAPTURING    The request is being sent. The server's
 *                             `stop_capture`: to AWAITING.
 *   AWAITING     THINKING     Waiting for the reply. `reply_start`: to
 *                             BUFFERING. `reply_end` (nothing was said):
 *                             to IDLE.
 *   BUFFERING    THINKING     Reply audio is arriving, none has played
 *                             yet. First sound: to PLAYING.
 *   PLAYING      SPEAKING     The reply is playing and more may come.
 *                             `reply_end`: to DRAINING.
 *   DRAINING     SPEAKING     The server has sent everything; the rest is
 *                             playing out. Last sample played: to IDLE.
 *   ERROR_HOLD   ERROR        A failure, shown for a few seconds. Then
 *                             wherever the device belongs.
 *   FAULT        ERROR        The device cannot hear or cannot speak.
 *                             Stays until that changes.
 *   REFUSED      SETUP        The server refused this device. Stays until
 *                             the settings are changed and it is rebooted.
 *
 * (BOOT, and SETUP for missing settings, are set by main.c before this
 * starts.)
 *
 * A turn cannot be interrupted: the wake word is acted on only in IDLE.
 *
 * The playback volume (volume.h) passes through here too. A new level from
 * the server or the console arrives as a message like everything else, so
 * that this task is the level's one writer, and the level is stored only in
 * IDLE, where writing to flash cannot interrupt a reply.
 *
 * When the device gives up on a turn by itself (a deadline passed, the
 * reply stopped arriving, something arrived that makes no sense), it closes
 * the connection with server_link_drop(). That is the protocol's rule for a device
 * in doubt: the server cancels the turn when the connection closes, and the
 * link opens a new one.
 */
#include "state_machine.h"

#include <stdatomic.h>
#include "app_state.h"
#include "audio_ring.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "protocol.h"
#include "upload.h"
#include "volume.h"
#include "wakeword.h"

static const char *TAG = "state_machine";

/* ---- Times that are this file's alone, in milliseconds. The protocol's
 * deadlines are in protocol.h. ---- */

/* How long a failure stays on the screen before the device moves on */
#define ERROR_HOLD_MS       4000
/* After a reply, detections are ignored for this long. The microphone
 * hears the speaker, and the last echoes of a reply must not start a
 * request. */
#define QUIET_AFTER_REPLY_MS 700
/* Longest wait for the player to finish once the server has sent
 * everything. What is left to play is at most the buffer; the rest is
 * margin. A guard against a missing report, not a time anyone should see. */
#define DRAIN_LIMIT_MS      (PROTO_REPLY_BUFFER_MS + 4000)
/* Longest wait for the chime to finish. It takes about a quarter
 * of a second; after this long the request starts without waiting further.
 * A guard against a missing report, like the one above. */
#define CHIME_LIMIT_MS        1000

/* Longest the task waits for a report before it checks in with the task
 * watchdog (see state_machine_task). The assertion holds it well inside the
 * watchdog's limit, which is set in sdkconfig.defaults, so that a change to
 * either is caught when the firmware is built. */
#define WATCHDOG_FEED_MS    1000
_Static_assert(WATCHDOG_FEED_MS * 2 <= CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
               "the task must check in well within the watchdog's limit");

/* For enter(): no deadline, or leave the current one as it is */
#define NO_DEADLINE         0
#define SAME_DEADLINE       UINT32_MAX

#define TASK_STACK          4096
/* Recorded in ARCHITECTURE.md's task table */
#define TASK_PRIO           5
#define TASK_CORE           0
/* Messages that can wait in the queue. Far more than ever do: the task
 * handles each in well under a millisecond. */
#define QUEUE_LENGTH        16

typedef enum {
    PH_CONNECTING,
    PH_IDLE,
    PH_CHIME,
    PH_CAPTURING,
    PH_AWAITING,
    PH_BUFFERING,
    PH_PLAYING,
    PH_DRAINING,
    PH_ERROR_HOLD,
    PH_FAULT,
    PH_REFUSED,
} phase_t;

/* Everything that can be shown on the screen's detail line. One table of
 * wording for all of it, below. */
typedef enum {
    WHY_NONE,
    /* no connection */
    WHY_NOT_FOUND,
    WHY_UNREACHABLE,
    WHY_INCOMPATIBLE,
    WHY_REPLACED,
    WHY_LOST,
    WHY_REFUSED,
    /* the server abandoned the turn */
    WHY_STT,
    WHY_LLM,
    WHY_TTS,
    WHY_SERVER,
    /* the device abandoned the turn */
    WHY_NO_ANSWER,
    WHY_STALLED,
    WHY_TOO_FAST,
    WHY_OUT_OF_STEP,
    WHY_NOT_SENT,
    WHY_INTERNAL,
    /* the device itself */
    WHY_MICROPHONE,
    WHY_SPEAKER,
    /* the last restart, when nobody asked for it */
    WHY_RESTART_SUPPLY,
    WHY_RESTART_WATCHDOG,
    WHY_RESTART_CRASH,
    WHY_COUNT
} why_t;

/* The screen's detail line fits about 38 characters (app_state.h). Each
 * entry is an array of exactly that size plus the terminating NUL, so the
 * compiler complains about a text that is too long. */
#define DETAIL_FITS         39
static const char WHY_TEXT[WHY_COUNT][DETAIL_FITS] = {
    [WHY_NONE]          = "",
    [WHY_NOT_FOUND]     = "Server not found on the network",
    [WHY_UNREACHABLE]   = "Cannot reach the server",
    [WHY_INCOMPATIBLE]  = "Server and device versions differ",
    [WHY_REPLACED]      = "Device ID in use by another device",
    [WHY_LOST]          = "Lost the server connection",
    [WHY_REFUSED]       = "Server refused this device",
    [WHY_STT]           = "Speech recognition failed",
    [WHY_LLM]           = "Claude did not answer",
    [WHY_TTS]           = "Speech synthesis failed",
    [WHY_SERVER]        = "Server error",
    [WHY_NO_ANSWER]     = "Server did not answer",
    [WHY_STALLED]       = "Reply stopped arriving",
    [WHY_TOO_FAST]      = "Reply arrived too fast",
    [WHY_OUT_OF_STEP]   = "Server out of step",
    [WHY_NOT_SENT]      = "Could not send the request",
    [WHY_INTERNAL]      = "Internal fault",
    [WHY_MICROPHONE]    = "Microphone not working",
    [WHY_SPEAKER]       = "Speaker not working",
    [WHY_RESTART_SUPPLY]   = "Restarted: supply dipped",
    [WHY_RESTART_WATCHDOG] = "Restarted: watchdog",
    [WHY_RESTART_CRASH]    = "Restarted: crash",
};

/* Everything that can happen */
typedef enum {
    EV_WAKE,            /* value: ring position where the phrase ended */
    EV_LISTENING,       /* whether the device can hear has changed; what it
                           is now is asked of wakeword_listening() */
    EV_LINK,            /* what: a server_link_report_t; value: as server_link.h says */
    EV_PLAYER,          /* what: a player_report_t; value: as player.h says */
    EV_UPLOAD_FAILED,   /* value: an upload_failure_t */
    EV_VOLUME,          /* from the console; value: a level already on the scale */
} ev_kind_t;

typedef struct {
    ev_kind_t kind;
    uint32_t what;
    uint32_t value;
} ev_t;

static QueueHandle_t s_events;
/* Set when a message could not be queued. Looked at by the state machine's
 * task, which then starts afresh: a lost message means it no longer knows
 * what state the rest of the firmware is in. */
static atomic_bool s_event_lost;
/* True while the device is in a turn and a reply may still come. Read by
 * the reply sink in the WebSocket client's task, which is why it is atomic
 * and not one of the plain variables below. */
static atomic_bool s_reply_wanted;

/* ---- Used only by the state machine's task ---- */

static phase_t s_phase = PH_CONNECTING;
/* Facts about the world, as last reported. They are not phases: a phase is
 * what the device is doing, these are what rest() chooses from. */
static bool s_link_up;          /* the server connection is ready */
static bool s_refused;          /* the server refused this device */
static bool s_speaker_ok;       /* the amplifier and player work */
/* Why the last attempt to connect failed, shown under CONNECTING while
 * the device keeps trying. Before the first attempt has ended it is
 * WHY_NONE, or, after a restart nobody asked for, the reason for that
 * restart (see restart_why). The first attempt's outcome then replaces it,
 * so it shows for a few seconds on a working network, and for as long as
 * there is no WiFi otherwise. */
static why_t s_attempt_why = WHY_NONE;
/* The current deadline, as a tick count (FreeRTOS's clock, in ticks since
 * boot), and whether there is one */
static bool s_deadline_set;
static TickType_t s_deadline;
/* After a reply, detections are ignored until this tick */
static bool s_quiet_set;
static TickType_t s_quiet_until;
/* Where in the microphone's stream the wake word ended (an audio_ring
 * position), kept to log how long after it the request began */
static uint32_t s_wake_pos;

/* ---- Building blocks ---- */

/* Plays at `level`, which is on the scale, from the next reply or chime
 * on. Stored at once in IDLE, and otherwise when the turn is over. */
static void change_volume(unsigned level)
{
    volume_set(level);
    if (s_phase == PH_IDLE) {
        volume_store();
    }
}

static bool in_turn(void)
{
    return s_phase == PH_CHIME || s_phase == PH_CAPTURING || s_phase == PH_AWAITING
        || s_phase == PH_BUFFERING || s_phase == PH_PLAYING || s_phase == PH_DRAINING;
}

/* The device state each phase is shown as. No `default`: the compiler then
 * warns when a phase is added and not given a state. */
static app_state_t shown_as(phase_t phase)
{
    switch (phase) {
    case PH_CONNECTING: return APP_STATE_CONNECTING;
    case PH_IDLE:       return APP_STATE_IDLE;
    /* The screen changes with the sound, not after it: both say "heard" */
    case PH_CHIME:
    case PH_CAPTURING:  return APP_STATE_CAPTURING;
    case PH_AWAITING:
    case PH_BUFFERING:  return APP_STATE_THINKING;
    case PH_PLAYING:
    case PH_DRAINING:   return APP_STATE_SPEAKING;
    case PH_ERROR_HOLD:
    case PH_FAULT:      return APP_STATE_ERROR;
    case PH_REFUSED:    return APP_STATE_SETUP;
    }
    return APP_STATE_ERROR;
}

/* A phase's name, for the log. No `default`, as above. */
static const char *phase_name(phase_t phase)
{
    switch (phase) {
    case PH_CONNECTING: return "connecting";
    case PH_IDLE:       return "idle";
    case PH_CHIME:      return "chime";
    case PH_CAPTURING:  return "capturing";
    case PH_AWAITING:   return "awaiting";
    case PH_BUFFERING:  return "buffering";
    case PH_PLAYING:    return "playing";
    case PH_DRAINING:   return "draining";
    case PH_ERROR_HOLD: return "error_hold";
    case PH_FAULT:      return "fault";
    case PH_REFUSED:    return "refused";
    }
    return "?";
}

/* Moves to a phase and shows it. The only place the device state is set.
 * `deadline_ms` is how long the phase may last, NO_DEADLINE, or
 * SAME_DEADLINE to keep the one already running. */
static void enter(phase_t phase, why_t why, uint32_t deadline_ms)
{
    s_phase = phase;
    if (deadline_ms == NO_DEADLINE) {
        s_deadline_set = false;
    } else if (deadline_ms != SAME_DEADLINE) {
        s_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(deadline_ms);
        s_deadline_set = true;
    }
    /* One line per change, stamped by the log with the time since boot:
     * the record that latency is measured from. It names the phase, with
     * the state on the screen in brackets, because several phases share a
     * state: "chime" is the wake word, "capturing" the start of the
     * request, "awaiting" its end, "playing" the reply's first sound. */
    ESP_LOGI(TAG, "%s [%s]%s%s", phase_name(phase), app_state_name(shown_as(phase)),
             WHY_TEXT[why][0] ? ": " : "", WHY_TEXT[why]);
    /* The phase is what the state machine goes by. If the announcement
     * fails, the screen is behind until the next change; nothing else is. */
    esp_err_t err = app_state_set(shown_as(phase), WHY_TEXT[why]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "state not announced: %s", esp_err_to_name(err));
    }
    if (phase == PH_IDLE) {
        /* A level changed during the turn, as "louder" does, is stored now
         * that nothing is playing */
        volume_store();
    }
}

/* Stops whatever a turn had running. Safe when nothing is. */
static void stop_turn_activity(void)
{
    atomic_store(&s_reply_wanted, false);
    upload_stop();
    player_abort();
}

/* What to say under CONNECTING after the chip's last restart: nothing after
 * an ordinary one (power-on, the reset button, the console's `reboot`), and
 * the cause after one nobody asked for. Without it such a restart looks
 * like any other power-on, and a device that restarts by itself now and
 * then would give no sign of why to someone not watching the serial log.
 * A brownout is the supply voltage dipping below what the chip needs
 * (KNOWN-ISSUES R2); the watchdogs restart a chip whose code has stuck; a
 * panic is a crash, whose state is kept in the core dump. */
static why_t restart_why(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:    return WHY_RESTART_SUPPLY;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:           return WHY_RESTART_WATCHDOG;
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP:    return WHY_RESTART_CRASH;
    default:                    return WHY_NONE;
    }
}

/* Goes to the phase the device belongs in when no turn is running and no
 * failure is being shown. The one place that order of precedence is
 * written down. */
static void rest(void)
{
    if (s_refused) {
        enter(PH_REFUSED, WHY_REFUSED, NO_DEADLINE);
    } else if (!wakeword_listening()) {
        /* Not IDLE: the screen must not say "waiting for the wake word"
         * on a device that cannot hear one. Asked of the wake-word
         * component each time, not kept here: it is the one that knows,
         * and a copy could be left wrong by a lost announcement. */
        enter(PH_FAULT, WHY_MICROPHONE, NO_DEADLINE);
    } else if (!s_speaker_ok) {
        enter(PH_FAULT, WHY_SPEAKER, NO_DEADLINE);
    } else if (s_link_up) {
        enter(PH_IDLE, WHY_NONE, NO_DEADLINE);
    } else {
        enter(PH_CONNECTING, s_attempt_why, NO_DEADLINE);
    }
}

/* Abandons the turn, if there is one, and shows why for a few seconds;
 * rest() follows when they have passed. With `drop`, also closes the
 * connection, which is how the server learns that the device has given up. */
static void fail(why_t why, bool drop)
{
    ESP_LOGW(TAG, "%s", WHY_TEXT[why]);
    stop_turn_activity();
    enter(PH_ERROR_HOLD, why, ERROR_HOLD_MS);
    if (drop) {
        server_link_drop();
    }
}

/* A message from the server that does not fit the phase the device is in.
 * While a turn could be under way that means the two sides disagree about
 * where they are, and the protocol's answer is to start afresh. Otherwise
 * it is the tail of a turn already given up, and is ignored. */
static void out_of_step(const char *what)
{
    if (s_phase == PH_IDLE || in_turn()) {
        ESP_LOGW(TAG, "unexpected %s", what);
        fail(WHY_OUT_OF_STEP, true);
    }
}

/* ---- What each message does ---- */

static void on_wake(uint32_t end_pos)
{
    if (s_phase != PH_IDLE) {
        ESP_LOGI(TAG, "wake word ignored: the device is busy or not ready");
        return;
    }
    /* Signed difference: tick counts wrap round, and this stays right */
    if (s_quiet_set && (int32_t)(xTaskGetTickCount() - s_quiet_until) < 0) {
        ESP_LOGI(TAG, "wake word ignored: too soon after the reply");
        return;
    }
    s_quiet_set = false;
    s_wake_pos = end_pos;
    /* The sound first, the request after it: the person hears that the
     * wake word worked and then speaks, and the microphone's copy of the
     * sound never reaches speech recognition. The player reports when the
     * sound is over; begin_request() follows. */
    player_chime();
    enter(PH_CHIME, WHY_NONE, CHIME_LIMIT_MS);
}

/* Starts sending the request, from this moment in the microphone's stream.
 * Anything said before now, during the chime, is not sent. */
static void begin_request(void)
{
    /* The player reports only after the amplifier has been fed silence and
     * switched off (audio_play_stop), some tens of milliseconds after the
     * last note. That pause is what keeps the note's tail out of the
     * request: shortening it changes what speech recognition hears. */
    uint32_t now = audio_ring_now();
    ESP_LOGI(TAG, "request begins %lu ms after the wake word",
             (unsigned long)((now - s_wake_pos) * 1000u / PROTO_CAPTURE_RATE_HZ));
    /* From now until the turn ends, reply audio is let through to the
     * player. Before the upload starts, so that nothing can arrive first. */
    atomic_store(&s_reply_wanted, true);
    upload_start(now, (uint8_t)volume_level());
    enter(PH_CAPTURING, WHY_NONE, PROTO_DEVICE_CAPTURE_LIMIT_MS);
}

static why_t why_down(server_link_down_t reason)
{
    switch (reason) {
    case SERVER_LINK_DOWN_NOT_FOUND:       return WHY_NOT_FOUND;
    case SERVER_LINK_DOWN_UNREACHABLE:     return WHY_UNREACHABLE;
    case SERVER_LINK_DOWN_REJECTED:        return WHY_REFUSED;
    case SERVER_LINK_DOWN_INCOMPATIBLE:    return WHY_INCOMPATIBLE;
    case SERVER_LINK_DOWN_REPLACED:        return WHY_REPLACED;
    case SERVER_LINK_DOWN_LOST:            return WHY_LOST;
    case SERVER_LINK_DOWN_CONFUSED:        return WHY_OUT_OF_STEP;
    case SERVER_LINK_DOWN_DROPPED:         return WHY_NONE;
    }
    return WHY_LOST;
}

static void on_link_down(server_link_down_t reason)
{
    bool was_up = s_link_up;
    s_link_up = false;
    if (reason == SERVER_LINK_DOWN_REJECTED) {
        s_refused = true;
        stop_turn_activity();
        rest();
        return;
    }
    if (reason == SERVER_LINK_DOWN_DROPPED) {
        /* The state machine closed it itself, and is already showing why */
        if (s_phase != PH_ERROR_HOLD) {
            rest();
        }
        return;
    }
    why_t why = why_down(reason);
    if (in_turn() || was_up) {
        /* A turn or a working connection was lost: shown as a failure for
         * a few seconds. What CONNECTING says afterwards starts empty,
         * except after being replaced: the link then waits a full minute
         * before trying again, and the screen should say why. */
        s_attempt_why = reason == SERVER_LINK_DOWN_REPLACED ? why : WHY_NONE;
        if (in_turn() || s_phase == PH_IDLE) {
            fail(why, false);
        }
        /* otherwise a failure or a fault is already on the screen, and
         * rest() will find the connection gone when its turn comes */
        return;
    }
    /* A failed attempt to connect. The link reports every one, with a
     * growing wait between them. It is not a new failure each time, so the
     * screen stays on CONNECTING and only the reason under it changes. */
    s_attempt_why = why;
    if (s_phase == PH_CONNECTING) {
        rest();
    }
}

static why_t why_error(server_link_error_t code)
{
    switch (code) {
    case SERVER_LINK_ERROR_STT:        return WHY_STT;
    case SERVER_LINK_ERROR_LLM:        return WHY_LLM;
    case SERVER_LINK_ERROR_TTS:        return WHY_TTS;
    case SERVER_LINK_ERROR_INTERNAL:
    case SERVER_LINK_ERROR_UNKNOWN:    return WHY_SERVER;
    }
    return WHY_SERVER;
}

static void on_link(server_link_report_t what, uint32_t value)
{
    switch (what) {
    case SERVER_LINK_UP:
        s_link_up = true;
        s_attempt_why = WHY_NONE;
        /* While a failure is on the screen it stays its few seconds; the
         * deadline's end then finds the connection up */
        if (s_phase == PH_CONNECTING) {
            rest();
        }
        break;
    case SERVER_LINK_DOWN:
        on_link_down((server_link_down_t)value);
        break;
    case SERVER_LINK_STOP_CAPTURE:
        if (s_phase == PH_CAPTURING) {
            upload_stop();
            enter(PH_AWAITING, WHY_NONE, PROTO_DEVICE_THINKING_LIMIT_MS);
        } else {
            out_of_step("stop_capture");
        }
        break;
    case SERVER_LINK_REPLY_START:
        if (s_phase == PH_AWAITING) {
            /* The sink has already started the player. The thinking
             * deadline runs on until the first sound comes out. */
            enter(PH_BUFFERING, WHY_NONE, SAME_DEADLINE);
        } else {
            out_of_step("reply_start");
        }
        break;
    case SERVER_LINK_REPLY_END:
        if (s_phase == PH_AWAITING) {
            /* Nothing was said after the wake word: no reply */
            atomic_store(&s_reply_wanted, false);
            rest();
        } else if (s_phase == PH_BUFFERING || s_phase == PH_PLAYING) {
            /* The rest plays out; the player's report ends the turn */
            player_end();
            enter(PH_DRAINING, WHY_NONE, DRAIN_LIMIT_MS);
        } else {
            out_of_step("reply_end");
        }
        break;
    case SERVER_LINK_SERVER_ERROR:
        if (in_turn()) {
            /* The connection is fine; only this turn is over */
            fail(why_error((server_link_error_t)value), false);
        }
        break;
    case SERVER_LINK_VOLUME:
        /* Taken in any phase. The server sends it while the device awaits
         * the reply that confirms it, but a level is harmless at any time,
         * and nothing is gained by refusing one. The player already has it
         * (state_machine_link_report); this records it. */
        change_volume(value);
        break;
    }
}

/* The turn is over and went well */
static void turn_done(void)
{
    atomic_store(&s_reply_wanted, false);
    s_quiet_until = xTaskGetTickCount() + pdMS_TO_TICKS(QUIET_AFTER_REPLY_MS);
    s_quiet_set = true;
    rest();
}

static void on_player(player_report_t what, uint32_t value)
{
    switch (what) {
    case PLAYER_STARTED:
        if (s_phase == PH_BUFFERING) {
            /* The player watches for a stalled reply itself from here */
            enter(PH_PLAYING, WHY_NONE, NO_DEADLINE);
        }
        /* In DRAINING the reply was so short that it ended before its
         * first sound; the screen already shows SPEAKING */
        break;
    case PLAYER_FINISHED:
        if (s_phase == PH_DRAINING) {
            turn_done();
        }
        break;
    case PLAYER_CHIME_DONE:
        if (s_phase == PH_CHIME) {
            if (value == 0) {
                /* The request goes ahead all the same. If the amplifier is
                 * really gone, the reply will fail and say so. */
                ESP_LOGW(TAG, "the chime could not be played");
            }
            begin_request();
        }
        break;
    case PLAYER_FAILED:
        if (!in_turn()) {
            break;      /* about a reply already abandoned */
        }
        switch ((player_failure_t)value) {
        case PLAYER_FAIL_STALLED:   fail(WHY_STALLED, true);    break;
        case PLAYER_FAIL_OVERFLOW:  fail(WHY_TOO_FAST, true);   break;
        case PLAYER_FAIL_DEVICE:    fail(WHY_SPEAKER, true);    break;
        }
        break;
    }
}

static void handle(const ev_t *ev)
{
    switch (ev->kind) {
    case EV_WAKE:
        on_wake(ev->value);
        break;
    case EV_LISTENING:
        /* Shown at once if nothing else is going on; otherwise rest()
         * shows it when the turn or the failure on the screen is over */
        if (s_phase == PH_IDLE || s_phase == PH_CONNECTING || s_phase == PH_FAULT) {
            rest();
        }
        break;
    case EV_LINK:
        on_link((server_link_report_t)ev->what, ev->value);
        break;
    case EV_PLAYER:
        on_player((player_report_t)ev->what, ev->value);
        break;
    case EV_UPLOAD_FAILED:
        if (s_phase == PH_CAPTURING) {
            fail(ev->value == UPLOAD_FAIL_MICROPHONE ? WHY_MICROPHONE : WHY_NOT_SENT, true);
        }
        break;
    case EV_VOLUME:
        change_volume(ev->value);
        break;
    }
}

static void on_deadline(void)
{
    s_deadline_set = false;
    switch (s_phase) {
    case PH_ERROR_HOLD:
        rest();
        break;
    case PH_CHIME:
        /* The sound is a courtesy; the request is what the person wants */
        ESP_LOGW(TAG, "no word from the player that the chime finished");
        /* Whatever it is still doing must not play into the request */
        player_abort();
        begin_request();
        break;
    case PH_CAPTURING:
    case PH_AWAITING:
    case PH_BUFFERING:
        fail(WHY_NO_ANSWER, true);
        break;
    case PH_DRAINING:
        /* The player should have reported by now. Whatever became of that
         * report, the reply is over. */
        ESP_LOGW(TAG, "no word from the player that the reply finished");
        player_abort();
        turn_done();
        break;
    default:
        break;
    }
}

static void state_machine_task(void *arg)
{
    /* The task watchdog is a timer in ESP-IDF that restarts the chip when a
     * task it watches has not checked in for the limit set in
     * sdkconfig.defaults, 5 s. This task asks to be watched, and checks in
     * on every pass of its loop. Every decision of the device is made
     * here, so if this task ever stuck the device would sit in whatever
     * state it last showed, deaf to everything; a restart is the remedy.
     * ESP_ERROR_CHECK: the request fails only if memory has run out at
     * boot, and a task that went on to check in without being watched
     * would have ESP-IDF log an error on every pass. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));    /* NULL = this task */

    for (;;) {
        esp_task_wdt_reset();   /* checks in with the watchdog */
        /* Waits for the next report until the current deadline, but never
         * longer than WATCHDOG_FEED_MS, so that the loop comes round to
         * check in even when nothing happens for hours. */
        TickType_t wait = pdMS_TO_TICKS(WATCHDOG_FEED_MS);
        if (s_deadline_set) {
            /* Signed, so that a deadline already past gives a negative
             * number, and so that the subtraction stays right when the
             * tick count wraps round */
            int32_t left = (int32_t)(s_deadline - xTaskGetTickCount());
            if (left < (int32_t)wait) {
                wait = left > 0 ? (TickType_t)left : 0;
            }
        }
        ev_t ev;
        if (xQueueReceive(s_events, &ev, wait) == pdTRUE) {
            handle(&ev);
        } else if (s_deadline_set && (int32_t)(s_deadline - xTaskGetTickCount()) <= 0) {
            /* The wait ran out, and the clock says it was the deadline
             * that passed and not only the watchdog's interval */
            on_deadline();
        }
        if (atomic_exchange(&s_event_lost, false)) {
            ESP_LOGE(TAG, "a report was lost; starting afresh");
            fail(WHY_INTERNAL, true);
        }
    }
}

/* ---- Getting messages into the queue ---- */

/* Called from other components' tasks. The short wait is for room in the
 * queue, which there always is unless the state machine's task has
 * stopped running. */
static void enqueue(ev_kind_t kind, uint32_t what, uint32_t value)
{
    ev_t ev = { .kind = kind, .what = what, .value = value };
    if (xQueueSend(s_events, &ev, pdMS_TO_TICKS(20)) != pdTRUE) {
        atomic_store(&s_event_lost, true);
    }
}

void state_machine_link_report(server_link_report_t what, uint32_t value)
{
    if (what == SERVER_LINK_VOLUME) {
        /* The player is given a new level here, in the link's task, and
         * not when this task gets round to the message: the reply that
         * confirms the level follows straight after, and its audio goes to
         * the player from the link's task too (state_machine_reply_start).
         * Done here, the level is in place before that audio can be, by
         * the order of the messages and not by luck. Setting the player's
         * level is safe from any task; recording and storing it is left to
         * this component's task. */
        player_set_gain_db(volume_db(value));
    }
    enqueue(EV_LINK, what, value);
}

void state_machine_player_report(player_report_t what, uint32_t value)
{
    enqueue(EV_PLAYER, what, value);
}

/* Runs in the upload task */
static void on_upload_failed(upload_failure_t why)
{
    enqueue(EV_UPLOAD_FAILED, 0, why);
}

/* Runs in the default event loop's task */
static void on_wakeword_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WAKEWORD_DETECTED) {
        enqueue(EV_WAKE, 0, ((const wakeword_event_t *)data)->end_pos);
    } else if (id == WAKEWORD_LISTENING) {
        enqueue(EV_LISTENING, 0, 0);
    }
}

/* Runs in the console's task */
esp_err_t state_machine_set_volume(unsigned level)
{
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(volume_on_scale(level), ESP_ERR_INVALID_ARG, TAG,
                        "volume off the scale");
    enqueue(EV_VOLUME, 0, level);
    return ESP_OK;
}

unsigned state_machine_volume(void)
{
    return volume_level();
}

/* The two functions below run in the WebSocket client's task */

void state_machine_reply_start(uint32_t sample_rate)
{
    if (atomic_load(&s_reply_wanted)) {
        player_begin(sample_rate);
    }
}

void state_machine_reply_data(const uint8_t *pcm, size_t len)
{
    if (atomic_load(&s_reply_wanted)) {
        player_feed(pcm, len);
    }
}

esp_err_t state_machine_start(const state_machine_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_events == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_speaker_ok = cfg->speaker_ok;
    s_attempt_why = restart_why(esp_reset_reason());

    /* The volume as it was left, given to the player before anything can
     * play */
    volume_init();

    s_events = xQueueCreate(QUEUE_LENGTH, sizeof(ev_t));
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(upload_init(on_upload_failed), TAG, "upload");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WAKEWORD_EVENT, ESP_EVENT_ANY_ID,
                                                   on_wakeword_event, NULL), TAG, "wake");

    /* Shows where the device stands from the start: CONNECTING, with the
     * reason for the restart if nobody asked for it, or the fault if it
     * cannot hear or speak. Done here, before the task exists, so nothing
     * else is touching the state. Whether it can hear is asked of the
     * wake-word component inside rest(), after the handler above was
     * registered: detection started earlier in the boot sequence and may
     * have found the microphone silent before the handler existed, and an
     * announcement made from now on waits in the queue for the task. */
    rest();
    BaseType_t made = xTaskCreatePinnedToCore(state_machine_task, "state_machine", TASK_STACK, NULL,
                                              TASK_PRIO, NULL, TASK_CORE);
    return made == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
