/*
 * The NV3007 display: SPI link, panel start-up, backlight and the display
 * task. What is drawn lives in display_ui.c, the test pattern in
 * display_test.c.
 *
 * The panel. The NV3007 is a display controller with its own memory (GRAM)
 * holding one screen of pixels. The chip sends it commands ("draw into this
 * rectangle") and pixel data over SPI, a fast serial bus: one bit per clock
 * pulse on MOSI, while CS is held low. The DC pin tells the controller
 * whether a byte is a command (low) or data (high). The panel can only
 * receive; there is no line back, so nothing here can check that it works
 * except looking at it. Pixels are RGB565: 16 bits each, 5 red, 6 green,
 * 5 blue, sent high byte first.
 *
 * LVGL. A graphics library: the program creates objects (labels, shapes)
 * and animations, and LVGL works out which rectangles of the screen changed,
 * draws them into a buffer in RAM, and hands each finished rectangle to a
 * "flush" function that sends it to the panel. Only the rectangles that
 * changed are sent, which is what makes animation affordable on this bus
 * (KNOWN-ISSUES R3). LVGL's NV3007 driver supplies the flush function and the
 * start-up commands; this file supplies the two functions it needs to reach
 * the panel: send a command, and send pixels.
 *
 * Memory. LVGL draws into one buffer the size of the whole screen, in PSRAM
 * (the 8 MB RAM chip in the package; internal RAM is faster but scarce).
 * The pixels are sent by DMA (direct memory access): the SPI hardware reads
 * the buffer by itself while the CPU is free. LVGL draws in the panel's byte
 * order (RGB565_SWAPPED), so no pass over the buffer is needed to reorder
 * bytes before sending.
 *
 * Orientation. The panel is natively portrait, 142 x 428. It is used in
 * landscape by setting its address mode (the MADCTL register) so that the
 * controller itself swaps rows and columns; LVGL just draws a 428 x 142
 * screen and costs nothing extra.
 *
 * Backlight. The BL pin switches the backlight through a transistor on the
 * module. It is driven by LEDC, the chip's PWM peripheral ("LED control"):
 * PWM switches the pin on and off tens of thousands of times a second, and
 * the fraction of time spent on (the duty) sets the brightness. LEDC can
 * also change the duty gradually by itself (a fade).
 *
 * Context. display_init(), display_test_start() and display_preview() run
 * in the caller's task.
 * Everything else runs in the display task, pinned to core 0 with the WiFi
 * work (ARCHITECTURE.md, task and core allocation), except on_color_done(),
 * which runs in the SPI interrupt, and on_state_event(), which runs in the
 * default event loop's task. Those two only wake the display task. The SPI
 * bus and the LEDC fade are set up from the display task, so their
 * interrupts run on core 0 too, away from the audio on core 1.
 */
#include "display.h"

#include <stdbool.h>
#include <string.h>
#include "app_state.h"
#include "board.h"
#include "display_priv.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "display";

/* Which way round landscape is. ROTATION_90 and ROTATION_270 are the two
 * landscape orientations, 180 degrees apart; 270 is the right way up for how
 * the device is mounted. */
#define ROTATION            LV_DISPLAY_ROTATION_270
/* The controller's memory is 168 columns wide and the panel shows 142 of
 * them, leaving 12 unused on one side and 14 on the other. In landscape
 * those columns run top to bottom, so the unused strip is an offset in y.
 * Measured on 2026-09-29 with the display test pattern: at ROTATION_270, 14
 * shows all four edges and 12 loses the top two rows. ROTATION_90 would
 * count from the other side and need 12. */
#define Y_OFFSET            14

/* TASK_STACK: 4.9 KB of this was the most used, measured with `display
 * test` after every state had been shown (2026-09-29). */
#define TASK_STACK          8192    /* bytes */
/* Priority and core: see ARCHITECTURE.md, task and core allocation */
#define TASK_PRIORITY       4
#define DISPLAY_CORE        0
/* Longest sleep between LVGL updates. LVGL says how long it can wait; this
 * caps it so a lost wake-up never freezes the screen for long. */
#define WAIT_MAX_MS         100
/* A full frame takes 12.5 ms; waiting this long for one to finish means
 * something is badly wrong, and it is worth saying so in the log */
#define FLUSH_WARN_MS       1000

/* Backlight PWM. 25 kHz is above hearing, so if the switching couples into
 * the audio wiring it cannot be heard; none was heard at 25 % duty
 * (2026-09-29). 10 bits gives duty values 0-1023. */
#define BL_FREQ_HZ          25000
#define BL_RESOLUTION       LEDC_TIMER_10_BIT
#define BL_DUTY_MAX         1023
#define BL_TIMER            LEDC_TIMER_0
#define BL_CHANNEL          LEDC_CHANNEL_0
#define BL_FADE_MS          300

/* Reasons to wake the display task, as bits of its task notification value */
#define WAKE_STATE          (1 << 0)    /* the device state changed */
#define WAKE_TEST           (1 << 1)    /* a test pattern was requested */
#define WAKE_PREVIEW        (1 << 2)    /* a preview of a state was requested */

static TaskHandle_t s_task;
static esp_lcd_panel_io_handle_t s_io;
static lv_display_t *s_disp;
/* Given by the SPI interrupt when a pixel transfer completes */
static SemaphoreHandle_t s_flush_done;
/* True once the panel works; read by display_test_start() in other tasks */
static volatile bool s_ready;

/* When the current pixel transfer started and ended, and its size. The end
 * is taken in the interrupt, the moment the last byte leaves. */
static int64_t s_flush_start_us;
static volatile int64_t s_flush_end_us;
static size_t s_flush_len;

/* ---- Backlight -------------------------------------------------------- */

/* Takes the backlight pin over at duty 0, so it is driven low (off) from
 * here on instead of floating (KNOWN-ISSUES R11). */
static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,  /* the only mode on the ESP32-S3 */
        .duty_resolution = BL_RESOLUTION,
        .timer_num = BL_TIMER,
        .freq_hz = BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,
    };
    return ledc_channel_config(&channel);
}

/* Fades to `percent` of full brightness over BL_FADE_MS, without waiting */
static void backlight_fade(int percent)
{
    /* A new fade would otherwise wait for a running one to finish, freezing
     * the display task for up to BL_FADE_MS; stopping it first lets the new
     * fade start from wherever the old one had got to */
    ledc_fade_stop(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
    uint32_t duty = (uint32_t)percent * BL_DUTY_MAX / 100;
    esp_err_t err = ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty,
                                                 BL_FADE_MS, LEDC_FADE_NO_WAIT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight fade: %s", esp_err_to_name(err));
    }
}

/* ---- Panel link, called by LVGL's NV3007 driver ------------------------ */

/* Runs in the SPI interrupt when the last byte of a pixel transfer has gone
 * out. An interrupt handler is a short function the hardware runs at once,
 * pausing whatever task was running on that core. IRAM_ATTR places this one
 * in internal RAM, which ESP-IDF recommends for code called from interrupts;
 * the SPI interrupt is masked while flash is being written, so this call
 * then simply comes a little later. */
static IRAM_ATTR bool on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    /* esp_timer_get_time() is itself in internal RAM, so safe here */
    s_flush_end_us = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    /* If that woke the display task, switch to it as the interrupt ends
     * rather than at the next scheduler tick, up to 10 ms later. The SPI
     * driver ignores this function's return value, so the switch is asked
     * for here. */
    portYIELD_FROM_ISR(woken);
    return false;
}

/* Sends a command byte and its parameters, and returns once they are sent.
 * The panel IO first waits for any pixel transfer still in progress, so
 * commands never overtake pixels. */
static void send_cmd(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                     const uint8_t *param, size_t param_size)
{
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, cmd[0], param, param_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "command 0x%02x: %s", cmd[0], esp_err_to_name(err));
    }
}

/* Starts sending a command followed by pixels, and returns without waiting;
 * flush_wait() waits for the end. */
static void send_color(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                       uint8_t *param, size_t param_size)
{
    s_flush_start_us = esp_timer_get_time();
    s_flush_len = param_size;
    esp_err_t err = esp_lcd_panel_io_tx_color(s_io, cmd[0], param, param_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pixels: %s", esp_err_to_name(err));
        /* Nothing was queued, so no completion interrupt will come; signal
         * it here so flush_wait() does not wait for one */
        s_flush_end_us = s_flush_start_us;
        xSemaphoreGive(s_flush_done);
    }
}

/* LVGL calls this after each send_color(), before touching the buffer
 * again. It sleeps until the transfer's interrupt, leaving the core free for
 * other tasks meanwhile. It never returns while the transfer may still be
 * running: DMA would then be reading the buffer while LVGL draws into it,
 * and the completion signal would arrive one transfer late ever after. */
static void flush_wait(lv_display_t *disp)
{
    while (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(FLUSH_WARN_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "pixel transfer still not finished");
    }
    test_frame_sent(s_flush_len, s_flush_end_us - s_flush_start_us);
}

/* ---- LVGL's links to the system ---------------------------------------- */

/* LVGL's clock: milliseconds since boot. It wraps after 49 days, which LVGL
 * handles. */
static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Sleeps instead of LVGL's default busy loop, so the pauses in the panel's
 * start-up leave the core to other tasks. The scheduler counts time in ticks
 * of 10 ms, and a sleep of n ticks ends at the n-th tick boundary, which can
 * be almost a whole tick early. So the time is rounded up to whole ticks and
 * one more is added: the panel's pauses are minimums. */
static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms + portTICK_PERIOD_MS - 1) + 1);
}

/* LVGL's warnings and errors, into the normal log */
static void lvgl_log(lv_log_level_t level, const char *buf)
{
    /* LVGL ends each message with a newline; the log adds its own */
    int len = (int)strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        len--;
    }
    ESP_LOGW("lvgl", "%.*s", len, buf);
}

/* ---- Start-up ----------------------------------------------------------- */

/* A failure part-way leaves what was already set up allocated. This runs
 * once at boot, the display task then ends and the screen stays dark, so
 * nothing is lost beyond some memory. */
static esp_err_t panel_start(void)
{
    /* RST is a plain digital output, not part of SPI; low holds the
     * controller in reset. Set high before it becomes an output, so the
     * controller is not reset twice. */
    gpio_set_level(BOARD_LCD_RST, 1);
    const gpio_config_t rst = {
        .pin_bit_mask = 1ULL << BOARD_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "reset pin");

    /* SPI controller 2. No MISO line (-1): the panel only receives. The
     * quad-SPI pins are unused. max_transfer_sz is the largest single
     * transfer: one full frame. */
    const spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");

    /* ESP-IDF's "panel IO" sends a command with DC low, then its parameters
     * or pixels with DC high. SPI mode 0 is the clock polarity and phase the
     * NV3007 expects; commands and parameters are 8 bits each. Up to 4
     * transfers may be queued. psram_dma_direct lets DMA read the frame
     * buffer in PSRAM directly; without it the driver would copy each
     * transfer into a temporary buffer in internal RAM first. */
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = SPI_CLOCK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.psram_dma_direct = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                 &io_cfg, &s_io), TAG, "panel IO");

    /* Hardware reset: hold RST low, release, then give the controller time
     * to start (times from the bring-up test, which worked) */
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_delay_set_cb(delay_ms);
    lv_log_register_print_cb(lvgl_log);

    /* Sends the NV3007 start-up sequence (vendor register values, then
     * sleep-out and display-on) through send_cmd(). Created in the panel's
     * native portrait size; the rotation below turns it to landscape. */
    s_disp = lv_nv3007_create(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, LV_LCD_FLAG_NONE,
                              send_cmd, send_color);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_ERR_NO_MEM, TAG, "LVGL display");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_rotation(s_disp, ROTATION);
    lv_nv3007_set_gap(s_disp, 0, Y_OFFSET);

    /* heap_caps_malloc() is malloc() with a choice of memory; MALLOC_CAP_SPIRAM
     * asks for PSRAM. DMA on the ESP32-S3 can read PSRAM at any address, and
     * the SPI driver writes the CPU's cached pixels out to PSRAM before each
     * transfer, so no special alignment is needed. */
    void *buf = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, TAG, "frame buffer");
    /* PARTIAL: LVGL redraws only changed rectangles, each placed at the
     * start of the buffer and sent on its own */
    lv_display_set_buffers(s_disp, buf, NULL, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_wait_cb(s_disp, flush_wait);

    /* The fade engine's interrupt, installed here so it runs on core 0 */
    ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), TAG, "backlight fade");
    return ESP_OK;
}

/* ---- The display task -------------------------------------------------- */

/* Runs in the default event loop's task. Only wakes the display task, which
 * reads the latest state from app_state itself: several changes in quick
 * succession then cost one redraw, of the newest. */
static void on_state_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xTaskNotify(s_task, WAKE_STATE, eSetBits);
}

/* A requested preview, handed from the requesting task to the display task
 * under a spinlock (a lock for very short sections; a task on the other
 * core waits by spinning) */
static portMUX_TYPE s_preview_lock = portMUX_INITIALIZER_UNLOCKED;
static app_state_event_t s_preview_req;
static uint32_t s_preview_req_seconds;
/* The running preview; display task only */
static bool s_previewing;
static uint32_t s_preview_end_ms;

/* Milliseconds since boot. Wraps after 49 days; the signed difference used
 * with it stays right across the wrap. */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void show_current_state(void)
{
    app_state_event_t st;
    app_state_get(&st);
    ui_show_state(&st);
    backlight_fade(ui_brightness(st.state));
}

static void display_task(void *arg)
{
    s_task = xTaskGetCurrentTaskHandle();
    esp_err_t err = panel_start();
    if (err == ESP_OK) {
        /* Subscribe before reading the current state: a change in between
         * is then delivered as an event rather than missed */
        err = esp_event_handler_register(APP_STATE_EVENT, APP_STATE_CHANGED,
                                         on_state_event, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display not available: %s", esp_err_to_name(err));
        vTaskDelete(NULL);  /* NULL: this task; does not return */
    }

    ui_init();
    app_state_event_t st;
    app_state_get(&st);
    ui_show_state(&st);
    lv_refr_now(s_disp);    /* draw and send the first frame... */
    backlight_fade(ui_brightness(st.state));    /* ...and only then light it */
    s_ready = true;
    ESP_LOGI(TAG, "ready, %dx%d", SCREEN_W, SCREEN_H);

    uint32_t wait_ms = 0;
    for (;;) {
        /* Sleeps until LVGL has work, or until woken with a reason. Always
         * at least one tick, so lower-priority tasks on core 0 get time. */
        uint32_t reasons = 0;
        TickType_t ticks = pdMS_TO_TICKS(wait_ms);
        xTaskNotifyWait(0, UINT32_MAX, &reasons, ticks ? ticks : 1);

        if ((reasons & WAKE_TEST) && !test_running()) {
            test_begin();
            backlight_fade(100);    /* full brightness, to judge the pixels */
        }
        if (test_running()) {
            /* A full frame on every pass; state changes wait until the end */
            if (test_tick()) {
                show_current_state();   /* the state may have changed meanwhile */
            } else {
                lv_refr_now(s_disp);
            }
        } else {
            if (reasons & WAKE_PREVIEW) {
                app_state_event_t st;
                taskENTER_CRITICAL(&s_preview_lock);
                st = s_preview_req;
                uint32_t seconds = s_preview_req_seconds;
                taskEXIT_CRITICAL(&s_preview_lock);
                ui_show_state(&st);
                backlight_fade(ui_brightness(st.state));
                s_previewing = true;
                s_preview_end_ms = now_ms() + seconds * 1000;
            }
            if (s_previewing && (int32_t)(now_ms() - s_preview_end_ms) >= 0) {
                s_previewing = false;
                show_current_state();   /* whatever the state has become */
            } else if ((reasons & WAKE_STATE) && !s_previewing) {
                show_current_state();
            }
        }

        /* Runs due animations and redraws what changed. Returns how long
         * until it next needs to run. */
        wait_ms = lv_timer_handler();
        if (test_running()) {
            wait_ms = 0;
        } else if (wait_ms > WAIT_MAX_MS) {
            wait_ms = WAIT_MAX_MS;
        }
    }
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_FALSE(s_flush_done == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    /* Backlight off first, before anything slow */
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done != NULL, ESP_ERR_NO_MEM, TAG, "semaphore");
    if (xTaskCreatePinnedToCore(display_task, "display", TASK_STACK, NULL,
                                TASK_PRIORITY, NULL, DISPLAY_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t display_test_start(uint32_t seconds, display_test_done_cb_t done)
{
    /* Plain returns, not logged: the console reports these to the user */
    if (seconds < 1 || seconds > 60) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || !test_request(seconds, done)) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_task, WAKE_TEST, eSetBits);
    return ESP_OK;
}

esp_err_t display_preview(app_state_t state, const char *detail, uint32_t seconds)
{
    if (state >= APP_STATE_COUNT || seconds < 1 || seconds > 60) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    app_state_event_t st = { .state = state };
    strlcpy(st.detail, detail ? detail : "", sizeof(st.detail));
    taskENTER_CRITICAL(&s_preview_lock);
    s_preview_req = st;
    s_preview_req_seconds = seconds;
    taskEXIT_CRITICAL(&s_preview_lock);
    xTaskNotify(s_task, WAKE_PREVIEW, eSetBits);
    return ESP_OK;
}
