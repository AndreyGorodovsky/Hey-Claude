/*
 * Wake-word detection with microWakeWord. See wakeword.h.
 *
 * This file is C++ because TensorFlow Lite for Microcontrollers (TFLite
 * Micro), the library that runs the model, is written in C++. Its interface
 * to the rest of the firmware, wakeword.h, is plain C.
 *
 * The pipeline, every 10 ms:
 *
 *   1. Audio. 160 new samples (10 ms) are read from the ring buffer
 *      (audio_ring.h).
 *   2. Features. The audio frontend turns the latest 30 ms of sound into 40
 *      numbers, one per frequency band, roughly how loud each band is: a
 *      "spectrogram" slice. It also evens out steady background noise and
 *      slow changes in loudness (noise reduction and PCAN, per-channel
 *      automatic gain control), which is why the quiet microphone level in
 *      KNOWN-ISSUES R12 matters less here than it might.
 *   3. Model. The model takes a few slices at a time (its "stride", 3 for
 *      the stock models, so it runs every 30 ms) and outputs one
 *      probability that the phrase has just ended. It is a "streaming"
 *      model: it keeps its own memory of earlier slices between runs, in
 *      TFLite "resource variables", so each run processes only the new
 *      slices rather than the last second of audio over again.
 *   4. Decision. The last few probabilities are averaged, and a detection
 *      is reported when the average exceeds the cutoff. After a detection,
 *      and after start-up, the detector is deaf for a second (COOLDOWN).
 *
 * The frontend settings and the feature scaling must match exactly what the
 * model was trained with. They are the settings every microWakeWord model
 * uses, taken from ESPHome's micro_wake_word component, the reference
 * implementation for these models.
 *
 * Numbers in the model are "quantised": stored as 8-bit integers standing
 * for real values, which makes the model four times smaller and much faster
 * than with floating point. The input is int8; the output probability is a
 * uint8 from 0 to 255, standing for 0 to 1. The cutoff is kept in the same
 * 0-255 form so the comparison needs no conversion.
 *
 * Memory. The model's weights (50-60 KB for the models in models/) are read
 * on every run; they are copied into PSRAM at start-up (see load_model() for
 * why). Its working memory, the "tensor arena" (23-26 KB), is in internal
 * RAM because it is read and written throughout every run, and PSRAM would
 * slow inference (ARCHITECTURE.md, memory placement).
 *
 * Context. wakeword_start() runs in the caller's task (main, at boot). The
 * detection loop runs in its own task on AUDIO_CORE (core 1). The stats and
 * settings functions may be called from any task; they share data with the
 * detection task under a spinlock, a lock for very short sections where a
 * task on the other core waits by spinning rather than sleeping.
 */
#include "wakeword.h"

#include <new>
#include <string.h>
#include "audio.h"
#include "audio_ring.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "frontend.h"
#include "frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "wakeword";

ESP_EVENT_DEFINE_BASE(WAKEWORD_EVENT);

/* The model file, embedded in the firmware by EMBED_FILES in CMakeLists.txt.
 * The build system defines these two symbols at its first and last byte. */
extern const uint8_t model_start[] asm("_binary_wakeword_model_tflite_start");
extern const uint8_t model_end[] asm("_binary_wakeword_model_tflite_end");

/* WAKEWORD_PHRASE, WAKEWORD_CUTOFF, WAKEWORD_WINDOW, WAKEWORD_STEP_MS and
 * WAKEWORD_ARENA come from the model's manifest, through CMakeLists.txt;
 * WAKEWORD_MODEL_NAME is the model's file name there, such as
 * "hey_claude_run3". */

/* --- Feature settings shared by every microWakeWord model --- */
#define FEATURES            40      /* frequency bands per slice */
#define FEATURE_WINDOW_MS   30      /* audio covered by one slice */
#define BAND_LOW_HZ         125.0f  /* lowest and highest frequency analysed */
#define BAND_HIGH_HZ        7500.0f
/* Noise reduction: how quickly the estimate of steady background noise
 * follows the sound, and how much of the signal it may remove at most */
#define NR_SMOOTHING_BITS   10
#define NR_EVEN_SMOOTHING   0.025f
#define NR_ODD_SMOOTHING    0.06f
#define NR_MIN_REMAINING    0.05f
/* PCAN, per-channel automatic gain control */
#define PCAN_STRENGTH       0.95f
#define PCAN_OFFSET         80.0f
#define PCAN_GAIN_BITS      21
#define LOG_SCALE_SHIFT     6

/* Converts a frontend output value to the model's int8 input. The frontend
 * gives values of roughly 0 to 670. Training divided them by 25.6 (giving
 * about 0 to 26) and quantisation then maps 0-26 onto -128 to 127. Together:
 * input = value * 256 / (25.6 * 26) - 128, with 25.6 * 26 = 665.6, rounded
 * to 666 so the sum stays in integers. */
#define FEATURE_SCALE_MUL   256
#define FEATURE_SCALE_DIV   666

/* Samples per 10 ms step, the unit the frontend advances by */
#define STEP_SAMPLES        (AUDIO_CAPTURE_RATE * WAKEWORD_STEP_MS / 1000)

/* Slices the detector stays deaf after start-up and after each detection:
 * 100 x 10 ms = 1 s. Stops one utterance being reported twice, and gives
 * the noise estimate time to settle at start-up. The count only advances
 * while the model's latest probability is below the cutoff, so the phrase
 * must also have ended. */
#define COOLDOWN_SLICES     100

/* Working memory for the model: the size its manifest gives, plus 10 %.
 * The need depends slightly on the version of the optimised kernels; those
 * are pinned (idf_component.yml), and with them the stock model was
 * measured to use 22,492 of the manifest's 22,860 bytes. The margin covers
 * a new model whose manifest is a little low. Too small an arena stops
 * start-up with an error rather than failing later; the real use is logged
 * at start-up and shown by `wake`. Updating the kernels means measuring
 * again. Rounded up to a multiple of 16 bytes, the alignment TFLite
 * expects. */
#define ARENA_SIZE          (((WAKEWORD_ARENA * 11 / 10) + 15) & ~15)
/* Separate, small arena for the streaming model's resource variables: its
 * memory of earlier slices. 1 KB and 20 variables are what ESPHome uses for
 * these models. */
#define VAR_ARENA_SIZE      1024
#define VAR_COUNT           20

/* Every kind of operation the microWakeWord models use. TFLite Micro links
 * in only the operations registered here; the number in angle brackets is
 * how many there are room for. */
#define OP_COUNT            20

/* Priority 8 (ARCHITECTURE.md, task table): below the capture task (12),
 * which must never be held up, and below the short-lived audio test tasks.
 * A run of the model takes a few milliseconds every 30 ms, so the task
 * sleeps most of the time and leaves room for everything below it. The
 * stack holds one step of audio and the model's own calls, plus logging. */
#define TASK_PRIORITY       8
#define TASK_STACK          6144    /* bytes; the high-water mark is in `wake` */

/* How long the task waits for audio before treating the capture as stalled */
#define READ_TIMEOUT_MS     1000

/* Slices between score logs when logging is on: 100 x 10 ms = 1 s */
#define LOG_EVERY_SLICES    100

/* Upper bound on the averaging window, for the array of recent results */
#define WINDOW_MAX          16
static_assert(WAKEWORD_WINDOW >= 1 && WAKEWORD_WINDOW <= WINDOW_MAX,
              "sliding_window_size in the model manifest is out of range");

/* --- Set up once by wakeword_start(), then used only by the detection
 * task --- */
static tflite::MicroMutableOpResolver<OP_COUNT> s_ops;
static tflite::MicroInterpreter *s_interp;
static FrontendState s_frontend;
static uint8_t *s_model;            /* aligned copy of the model, in PSRAM */
static uint8_t *s_arena;            /* tensor arena, internal RAM */
static uint8_t *s_var_arena;        /* resource variables, internal RAM */
static int s_stride;                /* slices the model takes per run */
static TaskHandle_t s_task;

/* --- Shared between the detection task and other tasks, under s_lock --- */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_cutoff;            /* 0-255 */
static bool s_log;
static bool s_stopped;              /* the model failed and detection ended */

/* The counters behind wakeword_stats_t, cleared by wakeword_reset_stats().
 * Settings and fixed facts are not kept here; wakeword_get_stats() adds
 * them to its copy. */
static struct {
    int64_t since_us;               /* when last cleared, microseconds since boot */
    uint32_t detections;
    float peak_score;
    uint32_t dropped;
    uint32_t inferences;
    uint64_t infer_us_total;        /* for the average */
    uint32_t infer_us_max;
} s_count;

/* Converts a 0-1 probability to the model's 0-255 form, rounding */
static uint8_t to_u8(float p)
{
    return (uint8_t)(p * 255.0f + 0.5f);
}

/* Registers the operations the models are built from. Returns false if the
 * resolver is full, which would mean OP_COUNT is too small. */
static bool register_ops(void)
{
    return s_ops.AddCallOnce() == kTfLiteOk
        && s_ops.AddVarHandle() == kTfLiteOk
        && s_ops.AddReshape() == kTfLiteOk
        && s_ops.AddReadVariable() == kTfLiteOk
        && s_ops.AddStridedSlice() == kTfLiteOk
        && s_ops.AddConcatenation() == kTfLiteOk
        && s_ops.AddAssignVariable() == kTfLiteOk
        && s_ops.AddConv2D() == kTfLiteOk
        && s_ops.AddMul() == kTfLiteOk
        && s_ops.AddAdd() == kTfLiteOk
        && s_ops.AddMean() == kTfLiteOk
        && s_ops.AddFullyConnected() == kTfLiteOk
        && s_ops.AddLogistic() == kTfLiteOk
        && s_ops.AddQuantize() == kTfLiteOk
        && s_ops.AddDepthwiseConv2D() == kTfLiteOk
        && s_ops.AddAveragePool2D() == kTfLiteOk
        && s_ops.AddMaxPool2D() == kTfLiteOk
        && s_ops.AddPad() == kTfLiteOk
        && s_ops.AddPack() == kTfLiteOk
        && s_ops.AddSplitV() == kTfLiteOk;
}

/* Sets up the audio frontend with the microWakeWord settings. Allocates a
 * few KB of buffers from the heap, kept for as long as the device runs. */
static esp_err_t init_frontend(void)
{
    FrontendConfig cfg;
    FrontendFillConfigWithDefaults(&cfg);
    cfg.window.size_ms = FEATURE_WINDOW_MS;
    cfg.window.step_size_ms = WAKEWORD_STEP_MS;
    cfg.filterbank.num_channels = FEATURES;
    cfg.filterbank.lower_band_limit = BAND_LOW_HZ;
    cfg.filterbank.upper_band_limit = BAND_HIGH_HZ;
    cfg.noise_reduction.smoothing_bits = NR_SMOOTHING_BITS;
    cfg.noise_reduction.even_smoothing = NR_EVEN_SMOOTHING;
    cfg.noise_reduction.odd_smoothing = NR_ODD_SMOOTHING;
    cfg.noise_reduction.min_signal_remaining = NR_MIN_REMAINING;
    cfg.pcan_gain_control.enable_pcan = 1;
    cfg.pcan_gain_control.strength = PCAN_STRENGTH;
    cfg.pcan_gain_control.offset = PCAN_OFFSET;
    cfg.pcan_gain_control.gain_bits = PCAN_GAIN_BITS;
    cfg.log_scale.enable_log = 1;
    cfg.log_scale.scale_shift = LOG_SCALE_SHIFT;
    /* Returns non-zero on success */
    ESP_RETURN_ON_FALSE(FrontendPopulateState(&cfg, &s_frontend, AUDIO_CAPTURE_RATE),
                        ESP_ERR_NO_MEM, TAG, "frontend");
    return ESP_OK;
}

/* Loads the model into the interpreter and checks that its input and output
 * have the shape this file expects. */
static esp_err_t load_model(void)
{
    /* The embedded file can sit at any address, but the model format
     * expects its data laid out on 16-byte boundaries, and some of its
     * values are read as 32-bit words, which this processor cannot read from
     * a misaligned address. EMBED_FILES does not promise any alignment, so
     * the model is copied into a block that starts on a 16-byte boundary.
     * PSRAM, because 50-60 KB is too much internal RAM for something only
     * read through the cache. */
    size_t size = model_end - model_start;
    s_model = (uint8_t *)heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_model != NULL, ESP_ERR_NO_MEM, TAG, "model copy");
    memcpy(s_model, model_start, size);

    const tflite::Model *model = tflite::GetModel(s_model);
    ESP_RETURN_ON_FALSE(model->version() == TFLITE_SCHEMA_VERSION, ESP_ERR_NOT_SUPPORTED,
                        TAG, "model format version %lu, expected %d",
                        (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
    ESP_RETURN_ON_FALSE(register_ops(), ESP_ERR_NO_MEM, TAG, "too many operations");

    /* MALLOC_CAP_INTERNAL: internal RAM only (see the top of this file) */
    s_arena = (uint8_t *)heap_caps_aligned_alloc(16, ARENA_SIZE,
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_var_arena = (uint8_t *)heap_caps_aligned_alloc(16, VAR_ARENA_SIZE,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_arena != NULL && s_var_arena != NULL, ESP_ERR_NO_MEM, TAG,
                        "tensor arena");

    /* The resource variables get an allocator of their own, so they are not
     * moved about as the main arena is planned. Both are created inside the
     * arena they manage, so they need no freeing. */
    tflite::MicroAllocator *var_alloc = tflite::MicroAllocator::Create(s_var_arena,
                                                                       VAR_ARENA_SIZE);
    ESP_RETURN_ON_FALSE(var_alloc != NULL, ESP_ERR_NO_MEM, TAG, "variable allocator");
    tflite::MicroResourceVariables *vars = tflite::MicroResourceVariables::Create(var_alloc,
                                                                                  VAR_COUNT);
    ESP_RETURN_ON_FALSE(vars != NULL, ESP_ERR_NO_MEM, TAG, "resource variables");

    /* std::nothrow: this firmware is built without C++ exceptions, so a
     * failed `new` must return NULL rather than throw */
    s_interp = new (std::nothrow) tflite::MicroInterpreter(model, s_ops, s_arena, ARENA_SIZE,
                                                           vars);
    ESP_RETURN_ON_FALSE(s_interp != NULL, ESP_ERR_NO_MEM, TAG, "interpreter");
    /* Plans where every intermediate value lives inside the arena; fails if
     * the arena is too small */
    ESP_RETURN_ON_FALSE(s_interp->AllocateTensors() == kTfLiteOk, ESP_ERR_NO_MEM, TAG,
                        "arena of %d bytes too small", ARENA_SIZE);

    /* Input: [1, stride, FEATURES] int8. Output: [1, 1] uint8. */
    TfLiteTensor *in = s_interp->input(0);
    TfLiteTensor *out = s_interp->output(0);
    ESP_RETURN_ON_FALSE(in->dims->size == 3 && in->dims->data[0] == 1
                        && in->dims->data[2] == FEATURES && in->type == kTfLiteInt8,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unexpected model input");
    ESP_RETURN_ON_FALSE(out->dims->size == 2 && out->dims->data[0] == 1
                        && out->dims->data[1] == 1 && out->type == kTfLiteUInt8,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unexpected model output");
    s_stride = in->dims->data[1];

    ESP_LOGI(TAG, "'%s' (%s): %u-byte model, stride %d (runs every %d ms), arena %u of %d "
                  "bytes used, cutoff %.3f, window %d",
             WAKEWORD_PHRASE, WAKEWORD_MODEL_NAME, (unsigned)size, s_stride,
             s_stride * WAKEWORD_STEP_MS,
             (unsigned)s_interp->arena_used_bytes(), ARENA_SIZE, s_cutoff / 255.0f,
             WAKEWORD_WINDOW);
    return ESP_OK;
}

/* State of the decision step (pipeline step 4), used only by the detection
 * task */
typedef struct {
    int slot;                       /* slices placed in the input so far this run */
    uint8_t recent[WINDOW_MAX];     /* the last WAKEWORD_WINDOW probabilities */
    int next;                       /* where the next probability goes in recent[] */
    uint8_t latest;                 /* the newest probability */
    int cooldown;                   /* slices left before detection is allowed */
    int log_slices;                 /* slices since the last score log */
    float log_peak;                 /* highest average since the last score log */
} detector_t;

static void detector_reset(detector_t *d)
{
    memset(d->recent, 0, sizeof(d->recent));
    d->latest = 0;
    d->cooldown = COOLDOWN_SLICES;
}

/* Runs the model; returns false if it failed */
static bool run_model(detector_t *d)
{
    int64_t t0 = esp_timer_get_time();     /* microseconds since boot */
    if (s_interp->Invoke() != kTfLiteOk) {
        return false;
    }
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);

    d->latest = s_interp->output(0)->data.uint8[0];
    d->recent[d->next] = d->latest;
    d->next = (d->next + 1) % WAKEWORD_WINDOW;

    taskENTER_CRITICAL(&s_lock);
    s_count.inferences++;
    s_count.infer_us_total += us;
    if (us > s_count.infer_us_max) {
        s_count.infer_us_max = us;
    }
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

/* Handles one slice of features from the frontend: steps 3 and 4 of the
 * pipeline. `end_pos` is the ring position just after the last sample the
 * slice covers. Returns false if the model failed to run. */
static bool handle_slice(detector_t *d, const FrontendOutput *f, uint32_t end_pos)
{
    /* Scale each value into the model's input, in this slice's place among
     * the `stride` slices of the next run */
    int8_t *in = s_interp->input(0)->data.int8 + d->slot * FEATURES;
    for (size_t i = 0; i < f->size; i++) {
        int32_t v = ((int32_t)f->values[i] * FEATURE_SCALE_MUL + FEATURE_SCALE_DIV / 2)
                    / FEATURE_SCALE_DIV;
        v += INT8_MIN;
        in[i] = (int8_t)(v < INT8_MIN ? INT8_MIN : v > INT8_MAX ? INT8_MAX : v);
    }

    bool ran = false;
    if (++d->slot >= s_stride) {
        d->slot = 0;
        if (!run_model(d)) {
            return false;
        }
        ran = true;
    }

    taskENTER_CRITICAL(&s_lock);
    uint8_t cutoff = s_cutoff;
    bool log = s_log;
    taskEXIT_CRITICAL(&s_lock);

    /* The cooldown counts down only while the model hears no phrase */
    if (d->cooldown > 0 && d->latest < cutoff) {
        d->cooldown--;
    }

    if (ran && d->cooldown == 0) {
        uint32_t sum = 0;
        for (int i = 0; i < WAKEWORD_WINDOW; i++) {
            sum += d->recent[i];
        }
        float avg = sum / (255.0f * WAKEWORD_WINDOW);
        if (avg > d->log_peak) {
            d->log_peak = avg;
        }
        /* The same test as avg > cutoff, kept in integers */
        bool detected = sum > (uint32_t)cutoff * WAKEWORD_WINDOW;

        taskENTER_CRITICAL(&s_lock);
        if (avg > s_count.peak_score) {
            s_count.peak_score = avg;
        }
        if (detected) {
            s_count.detections++;
        }
        taskEXIT_CRITICAL(&s_lock);

        if (detected) {
            ESP_LOGI(TAG, "detected '%s', score %.2f", WAKEWORD_PHRASE, avg);
            wakeword_event_t ev = { .score = avg, .end_pos = end_pos };
            /* Waits at most 10 ms for room in the event queue, so a stuck
             * event loop cannot stall listening */
            if (esp_event_post(WAKEWORD_EVENT, WAKEWORD_DETECTED, &ev, sizeof(ev),
                               pdMS_TO_TICKS(10)) != ESP_OK) {
                ESP_LOGW(TAG, "detection not announced: event queue full");
            }
            detector_reset(d);
        }
    }

    if (++d->log_slices >= LOG_EVERY_SLICES) {
        if (log) {
            ESP_LOGI(TAG, "peak score %.2f", d->log_peak);
        }
        d->log_slices = 0;
        d->log_peak = 0;
    }
    return true;
}

static void detect_task(void *arg)
{
    detector_t d = {};
    detector_reset(&d);
    audio_ring_reader_t reader;
    audio_ring_reader_init(&reader, 0);
    int16_t samples[STEP_SAMPLES];
    bool stalled = false;   /* logs each stall once */

    for (;;) {
        uint32_t dropped = 0;
        esp_err_t err = audio_ring_read(&reader, samples, STEP_SAMPLES, READ_TIMEOUT_MS,
                                        &dropped);
        if (err != ESP_OK) {
            if (!stalled) {
                ESP_LOGE(TAG, "no audio: %s", esp_err_to_name(err));
                stalled = true;
            }
            continue;   /* the read itself waited READ_TIMEOUT_MS */
        }
        stalled = false;
        if (dropped > 0) {
            taskENTER_CRITICAL(&s_lock);
            s_count.dropped += dropped;
            taskEXIT_CRITICAL(&s_lock);
        }

        /* The frontend keeps the last 30 ms internally and returns a slice
         * each time it has 10 ms of new audio. It may take fewer samples
         * than offered, so it is called until all are used. */
        size_t done = 0;
        while (done < STEP_SAMPLES) {
            size_t used = 0;
            FrontendOutput f = FrontendProcessSamples(&s_frontend, samples + done,
                                                      STEP_SAMPLES - done, &used);
            done += used;
            /* The reader has moved past the whole step; the slice ends
             * where the frontend stopped taking samples */
            uint32_t end_pos = reader.pos - (STEP_SAMPLES - done);
            /* f.values points into the frontend's own buffer, which the next
             * call overwrites, so the slice is handled before that */
            if (f.size == FEATURES && !handle_slice(&d, &f, end_pos)) {
                /* A model that has failed once will fail again; stopping is
                 * clearer than logging the same error every 30 ms */
                ESP_LOGE(TAG, "model failed to run; wake word detection stopped");
                taskENTER_CRITICAL(&s_lock);
                s_stopped = true;
                taskEXIT_CRITICAL(&s_lock);
                /* Suspended rather than deleted, so the task's handle stays
                 * valid for wakeword_get_stats() */
                vTaskSuspend(NULL);
            }
            if (used == 0) {
                break;  /* never expected; guards against looping forever */
            }
        }
    }
}

extern "C" esp_err_t wakeword_start(void)
{
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_cutoff = to_u8(WAKEWORD_CUTOFF);
    s_count.since_us = esp_timer_get_time();

    /* A failure here leaves what was already allocated in place. At boot that
     * costs some memory once, and detection simply does not run. */
    ESP_RETURN_ON_ERROR(init_frontend(), TAG, "frontend");
    ESP_RETURN_ON_ERROR(load_model(), TAG, "model");

    if (xTaskCreatePinnedToCore(detect_task, "wakeword", TASK_STACK, NULL, TASK_PRIORITY,
                                &s_task, AUDIO_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" const char *wakeword_phrase(void)
{
    return WAKEWORD_PHRASE;
}

extern "C" const char *wakeword_model(void)
{
    return WAKEWORD_MODEL_NAME;
}

extern "C" esp_err_t wakeword_set_cutoff(float cutoff)
{
    /* In the 0-255 form, 0 would let any sound through and 255 could never
     * be exceeded, so both are refused; that leaves about 0.002 to 0.998 */
    uint8_t c = cutoff > 0.0f && cutoff < 1.0f ? to_u8(cutoff) : 0;
    ESP_RETURN_ON_FALSE(c > 0 && c < 255, ESP_ERR_INVALID_ARG, TAG,
                        "cutoff must be between 0.002 and 0.998");
    taskENTER_CRITICAL(&s_lock);
    s_cutoff = c;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

extern "C" void wakeword_set_log(bool on)
{
    taskENTER_CRITICAL(&s_lock);
    s_log = on;
    taskEXIT_CRITICAL(&s_lock);
}

extern "C" void wakeword_get_stats(wakeword_stats_t *out)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    *out = wakeword_stats_t{};
    /* Microseconds to seconds */
    out->seconds = (uint32_t)((now - s_count.since_us) / 1000000);
    out->detections = s_count.detections;
    out->peak_score = s_count.peak_score;
    out->dropped = s_count.dropped;
    out->inferences = s_count.inferences;
    out->infer_us_avg = s_count.inferences
                        ? (uint32_t)(s_count.infer_us_total / s_count.inferences) : 0;
    out->infer_us_max = s_count.infer_us_max;
    out->cutoff = s_cutoff / 255.0f;
    out->running = s_task != NULL && !s_stopped;
    taskEXIT_CRITICAL(&s_lock);
    /* Set once at start-up, so read outside the lock */
    out->arena_used = s_interp ? s_interp->arena_used_bytes() : 0;
    out->arena_size = s_interp ? ARENA_SIZE : 0;
    /* In ESP-IDF this reports bytes, not words as in standard FreeRTOS */
    out->stack_unused = s_task ? uxTaskGetStackHighWaterMark(s_task) : 0;
}

extern "C" void wakeword_reset_stats(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_count = {};
    s_count.since_us = now;
    taskEXIT_CRITICAL(&s_lock);
}
