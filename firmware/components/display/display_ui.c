/*
 * What the display shows: one view per device state, and a test pattern.
 *
 * The screen is 428 x 142 (landscape). The state view has a square
 * animation area on the left and text on the right: the state in large type
 * and, when there is one, a detail line below it. A state may add lines of
 * its own between the two, built by its entry in the table below: setup
 * mode (the setup component) shows there the hotspot to join, its password,
 * the page to open, and the minutes left. The text is centred
 * vertically whether it has one line or two. While continuous conversation
 * is on, a small mark in the top right corner says so, on the screens where
 * it matters: waiting and listening. Only the animation area moves,
 * so each frame redraws a rectangle of at most 142 x 142 rather than the
 * whole screen (KNOWN-ISSUES R3).
 *
 * The animations are placeholders, kept as they are (settled in stage 8).
 * Each is built from LVGL objects (circles are rectangles with fully rounded corners) and
 * LVGL animations: an animation moves one number, such as an opacity or a
 * height, between two values over a set time, calling a function with each
 * new value; LVGL redraws whatever that changed. Deleting an object stops
 * its animations with it, which is how switching state clears the old one.
 *
 * Context: every function runs in the display task (display.c), the only
 * task that uses LVGL.
 */
#include "display_priv.h"

#include <stdbool.h>
#include <string.h>
#include "esp_timer.h"

#define ICON_SIZE       SCREEN_H    /* square animation area on the left */
#define TEXT_X          (ICON_SIZE + 8)
#define TEXT_W          (SCREEN_W - TEXT_X - 8)

/* Colours as 0xRRGGBB */
#define COLOR_BG        0x000000
#define COLOR_ACCENT    0xD97757    /* warm orange */
#define COLOR_TEXT      0xFFFFFF
#define COLOR_DIM_TEXT  0x9A9A9A
#define COLOR_TRACK     0x303030    /* the spinner's background ring */
#define COLOR_ERROR     0xE5484D

static lv_obj_t *s_state_scr, *s_icon, *s_title, *s_detail;
/* The continuous conversation mark, hidden unless shown */
static lv_obj_t *s_mode_mark;
/* The state and detail the view was last built for, so that a change of
 * the mode alone does not build it again; and a request to build it again
 * all the same */
static bool s_drawn_valid;
static bool s_redraw;
static app_state_t s_drawn_state;
static char s_drawn_detail[APP_STATE_DETAIL_MAX];
/* The column a state's own lines go in, between title and detail */
static lv_obj_t *s_lines;
/* The setup screen's details, and whether they have arrived */
static display_setup_t s_setup;
static bool s_has_setup;
/* The setup screen's countdown and the LVGL timer that updates it, while
 * the setup view is on the screen; NULL otherwise */
static lv_obj_t *s_countdown;
static lv_timer_t *s_countdown_timer;
static lv_obj_t *s_test_scr, *s_test_bar;

/* ---- Building blocks ---------------------------------------------------- */

/* A filled circle of diameter `d`, centred in `parent` and then moved by
 * (x, y). The alignment is remembered, so the circle stays centred when an
 * animation changes its size. */
static lv_obj_t *circle(lv_obj_t *parent, int32_t d, uint32_t rgb, int32_t x, int32_t y)
{
    /* remove_style_all drops the default theme's look (border, padding,
     * background), leaving a blank object to style from scratch */
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_align(o, LV_ALIGN_CENTER, x, y);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    return l;
}

/* Functions animations call with each new value */
static void set_bg_opa(void *o, int32_t v)     { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }
static void set_border_opa(void *o, int32_t v) { lv_obj_set_style_border_opa(o, (lv_opa_t)v, 0); }
static void set_size(void *o, int32_t v)       { lv_obj_set_size(o, v, v); }
static void set_height(void *o, int32_t v)     { lv_obj_set_height(o, v); }

/* Starts an endless animation of `obj` from `from` to `to` over `ms`
 * milliseconds. With `reverse`, each run goes back again over the same time
 * before repeating; without, it jumps back to `from`. `delay_ms` holds off
 * only the first run, which staggers several animations of the same length. */
static void animate(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                    uint32_t ms, uint32_t delay_ms, bool reverse)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay_ms);
    if (reverse) {
        lv_anim_set_reverse_duration(&a, ms);
    }
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    /* Slow at both ends, faster in the middle, which looks less mechanical */
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);  /* copies `a`, so a local is fine */
}

/* ---- One animation per state ------------------------------------------- */

static void build_boot(void)
{
    circle(s_icon, 40, COLOR_ACCENT, 0, 0);
}

/* A still ring: nothing moves until settings are entered */
static void build_setup(void)
{
    lv_obj_t *ring = circle(s_icon, 56, COLOR_ACCENT, 0, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(ring, 4, 0);
}

/* Three dots brightening one after another */
static void build_connecting(void)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = circle(s_icon, 14, COLOR_ACCENT, (i - 1) * 26, 0);
        animate(dot, set_bg_opa, LV_OPA_20, LV_OPA_COVER, 500, i * 200, true);
    }
}

/* A dim dot, slowly breathing */
static void build_idle(void)
{
    lv_obj_t *dot = circle(s_icon, 28, COLOR_ACCENT, 0, 0);
    animate(dot, set_bg_opa, LV_OPA_20, LV_OPA_70, 1600, 0, true);
}

/* A ring spreading out and fading from a solid centre */
static void build_capturing(void)
{
    lv_obj_t *ring = circle(s_icon, 50, COLOR_ACCENT, 0, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(ring, 4, 0);
    animate(ring, set_size, 40, 110, 900, 0, false);
    animate(ring, set_border_opa, LV_OPA_COVER, LV_OPA_TRANSP, 900, 0, false);
    circle(s_icon, 24, COLOR_ACCENT, 0, 0);
}

/* A rotating arc on a dark track */
static void build_thinking(void)
{
    lv_obj_t *sp = lv_spinner_create(s_icon);
    lv_obj_set_size(sp, 70, 70);
    lv_obj_center(sp);
    lv_spinner_set_anim_params(sp, 1000, 270);  /* one turn per second, 270 degree arc */
    lv_obj_set_style_arc_color(sp, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 6, LV_PART_INDICATOR);
}

/* Five bars rising and falling out of step, like a level meter. Their
 * timing is invented; real levels arrive with playback in stage 6. */
static void build_speaking(void)
{
    static const uint16_t period_ms[5] = { 300, 420, 360, 480, 390 };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *bar = circle(s_icon, 10, COLOR_ACCENT, (i - 2) * 20, 0);
        lv_obj_set_style_radius(bar, 5, 0);
        animate(bar, set_height, 14, 70, period_ms[i], 0, true);
    }
}

/* A red disc with an exclamation mark; the detail line says what failed */
static void build_error(void)
{
    lv_obj_t *disc = circle(s_icon, 60, COLOR_ERROR, 0, 0);
    lv_obj_t *mark = lv_label_create(disc);
    lv_label_set_text(mark, "!");
    lv_obj_set_style_text_font(mark, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(mark, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(mark);
}

/* ---- Lines of a state's own ------------------------------------------- */

/* Shows the minutes left before setup mode closes, rounded up, so that the
 * last minute reads "1 min". Runs once a second while the setup view is up,
 * from an LVGL timer, which LVGL calls in the display task. */
static void update_countdown(lv_timer_t *timer)
{
    int64_t left_us = s_setup.closes_at_us - esp_timer_get_time();
    if (left_us <= 0) {
        lv_label_set_text(s_countdown, "Closing");
    } else {
        lv_label_set_text_fmt(s_countdown, "Closes in %d min",
                              (int)((left_us + 59999999) / 60000000));
    }
}

/* Setup mode: how to reach the setup page, and when setup mode closes by
 * itself, if it does. White at 14 px: the password is read off the screen
 * and typed into a phone, and the detail line's dim grey is harder to read.
 * Nothing before the details arrive. */
static void build_setup_lines(lv_obj_t *box)
{
    if (!s_has_setup) {
        return;
    }
    lv_label_set_text_fmt(label(box, &lv_font_montserrat_14, COLOR_TEXT),
                          "Join WiFi  %s", s_setup.ssid);
    lv_label_set_text_fmt(label(box, &lv_font_montserrat_14, COLOR_TEXT),
                          "Password  %s", s_setup.password);
    lv_label_set_text_fmt(label(box, &lv_font_montserrat_14, COLOR_TEXT),
                          "Then open  %s", s_setup.address);
    if (s_setup.closes_at_us != 0) {
        s_countdown = label(box, &lv_font_montserrat_14, COLOR_DIM_TEXT);
        update_countdown(NULL);
        s_countdown_timer = lv_timer_create(update_countdown, 1000, NULL);
    }
}

/* Everything about how each state looks, in one place: the large title,
 * the detail line shown when the state carries no detail of its own, the
 * animation, any lines of its own under the title, and the backlight
 * brightness in percent. A state missing from this table would leave its
 * entry empty; app_state.c's build-time check on the number of states is
 * the reminder to add it here. */
static const struct {
    const char *title;
    const char *detail;
    void (*build)(void);
    void (*lines)(lv_obj_t *box);   /* NULL for none */
    int brightness;
    bool marks_mode;    /* shows the continuous conversation mark when on */
} s_views[APP_STATE_COUNT] = {
    [APP_STATE_BOOT]       = { "Hey Claude",   "Starting",          build_boot,       NULL,              100, false },
    [APP_STATE_SETUP]      = { "Setup needed", "",                  build_setup,      build_setup_lines, 100, false },
    [APP_STATE_CONNECTING] = { "Connecting",   "",                  build_connecting, NULL,              100, false },
    /* Dimmed while waiting for the wake word, which is most of the time */
    [APP_STATE_IDLE]       = { "Ready",        "Say the wake word", build_idle,       NULL,              25,  true },
    [APP_STATE_CAPTURING]  = { "Listening",    "",                  build_capturing,  NULL,              100, true },
    [APP_STATE_THINKING]   = { "Thinking",     "",                  build_thinking,   NULL,              100, false },
    [APP_STATE_SPEAKING]   = { "Speaking",     "",                  build_speaking,   NULL,              100, false },
    [APP_STATE_ERROR]      = { "Error",        "",                  build_error,      NULL,              100, false },
};

/* ---- Screens ------------------------------------------------------------ */

/* A black screen with no scrolling */
static lv_obj_t *screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);    /* NULL parent: a screen */
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);
    return scr;
}

static void build_state_screen(void)
{
    s_state_scr = screen();

    s_icon = lv_obj_create(s_state_scr);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, ICON_SIZE, ICON_SIZE);
    lv_obj_align(s_icon, LV_ALIGN_LEFT_MID, 0, 0);

    /* The title and detail stack in a column that is exactly as tall as its
     * contents and centred vertically. "Flex" is LVGL's layout that places
     * children one after another; a hidden child takes no space, so hiding
     * an empty detail line re-centres the title on its own. */
    lv_obj_t *text = lv_obj_create(s_state_scr);
    lv_obj_remove_style_all(text);
    lv_obj_set_size(text, TEXT_W, LV_SIZE_CONTENT);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, TEXT_X, 0);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, 4, 0);   /* pixels between the lines */

    s_title = label(text, &lv_font_montserrat_28, COLOR_TEXT);

    /* A state's own lines, a column of their own inside the text column,
     * hidden while empty so that it takes no space */
    s_lines = lv_obj_create(text);
    lv_obj_remove_style_all(s_lines);
    lv_obj_set_size(s_lines, TEXT_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_lines, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_lines, 2, 0);
    lv_obj_set_hidden(s_lines, true);

    s_detail = label(text, &lv_font_montserrat_14, COLOR_DIM_TEXT);
    lv_obj_set_width(s_detail, TEXT_W);
    /* Text too long for one line ends in "..." instead of wrapping */
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_MODE_DOTS);

    /* The continuous conversation mark: LVGL's "loop" symbol, which its
     * built-in fonts carry alongside the letters, and a word, in the corner
     * that the centred text never reaches. It does not move, so it costs no
     * redrawing once drawn. */
    s_mode_mark = label(s_state_scr, &lv_font_montserrat_14, COLOR_ACCENT);
    lv_label_set_text(s_mode_mark, LV_SYMBOL_LOOP " Continuous");
    lv_obj_align(s_mode_mark, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_hidden(s_mode_mark, true);
}

/* A 1-pixel frame, one colour per edge, so a missing edge is identifiable:
 * top red, bottom green, left blue, right white. Corner labels show
 * whether the picture is the right way round. A bar sweeps across. */
static void build_test_screen(void)
{
    static const struct { int32_t x, y, w, h; uint32_t rgb; } edges[] = {
        { 0, 0, SCREEN_W, 1, 0xFF0000 },
        { 0, SCREEN_H - 1, SCREEN_W, 1, 0x00FF00 },
        { 0, 0, 1, SCREEN_H, 0x0000FF },
        { SCREEN_W - 1, 0, 1, SCREEN_H, 0xFFFFFF },
    };
    static const struct { const char *text; lv_align_t align; int32_t x, y; } corners[] = {
        { "top left", LV_ALIGN_TOP_LEFT, 4, 4 },
        { "top right", LV_ALIGN_TOP_RIGHT, -4, 4 },
        { "bottom left", LV_ALIGN_BOTTOM_LEFT, 4, -4 },
        { "bottom right", LV_ALIGN_BOTTOM_RIGHT, -4, -4 },
    };

    s_test_scr = screen();
    s_test_bar = lv_obj_create(s_test_scr);
    lv_obj_remove_style_all(s_test_bar);
    lv_obj_set_style_bg_color(s_test_bar, lv_color_hex(0x505050), 0);
    lv_obj_set_style_bg_opa(s_test_bar, LV_OPA_COVER, 0);
    lv_obj_set_size(s_test_bar, 24, SCREEN_H - 2);
    lv_obj_set_pos(s_test_bar, 1, 1);

    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        lv_obj_t *e = lv_obj_create(s_test_scr);
        lv_obj_remove_style_all(e);
        lv_obj_set_style_bg_color(e, lv_color_hex(edges[i].rgb), 0);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
        lv_obj_set_pos(e, edges[i].x, edges[i].y);
        lv_obj_set_size(e, edges[i].w, edges[i].h);
    }
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        lv_obj_t *l = label(s_test_scr, &lv_font_montserrat_14, COLOR_TEXT);
        lv_label_set_text(l, corners[i].text);
        lv_obj_align(l, corners[i].align, corners[i].x, corners[i].y);
    }
    lv_obj_t *l = label(s_test_scr, &lv_font_montserrat_28, COLOR_TEXT);
    lv_label_set_text(l, "display test");
    lv_obj_center(l);
}

/* ---- Interface ---------------------------------------------------------- */

void ui_init(void)
{
    build_state_screen();
    build_test_screen();
    lv_screen_load(s_state_scr);
}

void ui_show_state(const app_state_event_t *st)
{
    if (st->state >= APP_STATE_COUNT || s_views[st->state].build == NULL) {
        return;
    }
    /* Only the mark changes when only the mode has: rebuilding the view
     * would restart its animation for no visible reason, for instance on
     * the SPEAKING screen when the switch is moved during a reply */
    lv_obj_set_hidden(s_mode_mark, !(st->continuous && s_views[st->state].marks_mode));
    if (s_drawn_valid && !s_redraw && st->state == s_drawn_state
            && strcmp(st->detail, s_drawn_detail) == 0) {
        return;
    }
    s_drawn_valid = true;
    s_redraw = false;
    s_drawn_state = st->state;
    strlcpy(s_drawn_detail, st->detail, sizeof(s_drawn_detail));

    lv_obj_clean(s_icon);   /* deletes the old animation's objects */
    s_views[st->state].build();
    lv_label_set_text(s_title, s_views[st->state].title);
    /* The old view's lines go, and its timer with them: a timer is not a
     * child of any object, so deleting the lines would not stop it */
    if (s_countdown_timer != NULL) {
        lv_timer_delete(s_countdown_timer);
        s_countdown_timer = NULL;
    }
    s_countdown = NULL;
    lv_obj_clean(s_lines);
    if (s_views[st->state].lines != NULL) {
        s_views[st->state].lines(s_lines);
    }
    lv_obj_set_hidden(s_lines, lv_obj_get_child_count(s_lines) == 0);
    const char *detail = st->detail[0] ? st->detail : s_views[st->state].detail;
    lv_label_set_text(s_detail, detail);
    lv_obj_set_hidden(s_detail, detail[0] == '\0');
}

void ui_set_setup(const display_setup_t *setup)
{
    s_setup = *setup;
    s_has_setup = true;
    /* The setup view's lines are built from these, so it must be built
     * again even though its state and detail have not changed */
    s_redraw = true;
}

int ui_brightness(app_state_t state)
{
    return state < APP_STATE_COUNT ? s_views[state].brightness : 100;
}

void ui_test_begin(void)
{
    lv_screen_load(s_test_scr);
}

void ui_test_step(void)
{
    /* 6 pixels per frame; wraps back to the left edge */
    int32_t x = lv_obj_get_x(s_test_bar) + 6;
    lv_obj_set_x(s_test_bar, x > SCREEN_W - 25 ? 1 : x);
    lv_obj_invalidate(s_test_scr);
}

void ui_test_end(void)
{
    lv_screen_load(s_state_scr);
}
