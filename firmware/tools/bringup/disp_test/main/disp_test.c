/*
 * Bring-up test: NV3007 display on SPI2 (SCLK=12, MOSI=11, CS=10, DC=9,
 * RST=8, BL=14).
 *
 * Keeps the backlight off for 3 s, switches it on, initialises the panel,
 * then fills the whole screen red, green, blue, white and black in a 2 s
 * loop. Correct colours in that order confirm every wire; the panel cannot
 * send anything back, so looking at it is the only check.
 *
 * Background: the display is driven over SPI, a fast serial bus. The chip
 * sends bytes on MOSI, one bit per SCLK pulse, while CS is low. The DC pin
 * says what the bytes are: low = a command (for example "set drawing window"),
 * high = data (the command's parameters, or pixels). Pixels are RGB565:
 * 16 bits each, 5 bits red, 6 green, 5 blue.
 *
 * The panel controller's memory (GRAM) is 168 x 428 pixels, wider than the
 * 142 visible columns. This test fills all of it, so the column offset of the
 * visible area does not matter here; the real driver must account for it.
 *
 * The initialisation sequence is copied from LVGL's lv_nv3007.c driver,
 * itself ported from Arduino_GFX. This is a standalone ESP-IDF project, not
 * part of the device firmware.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"

#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_CS   10
#define PIN_DC   9
#define PIN_RST  8
#define PIN_BL   14

#define GRAM_W 168      /* controller memory width in pixels */
#define GRAM_H 428      /* controller memory height in pixels */
#define LINES  20       /* rows sent per SPI transfer: 168 * 20 * 2 = 6.7 KB */

/*
 * Vendor register settings (power, voltages, gamma, timing). Opaque by
 * nature: the values come from the panel maker. Format, repeated: command
 * byte, number of parameter bytes, then the parameters. For example
 * "0x8f,2,0x55,0x04" sends command 0x8F with parameters 0x55 and 0x04.
 * These registers are only reachable after an unlock command (0xFF, 0xA5),
 * sent before this list in app_main.
 */
static const uint8_t init1[] = {
    0x9a,1,0x08, 0x9b,1,0x08, 0x9c,1,0xb0, 0x9d,1,0x16, 0x9e,1,0xc4,
    0x8f,2,0x55,0x04, 0x84,1,0x90, 0x83,1,0x7b, 0x85,1,0x33,
    0x60,1,0x00, 0x70,1,0x00, 0x61,1,0x02, 0x71,1,0x02, 0x62,1,0x04, 0x72,1,0x04,
    0x6c,1,0x29, 0x7c,1,0x29, 0x6d,1,0x31, 0x7d,1,0x31, 0x6e,1,0x0f, 0x7e,1,0x0f,
    0x66,1,0x21, 0x76,1,0x21, 0x68,1,0x3A, 0x78,1,0x3A, 0x63,1,0x07, 0x73,1,0x07,
    0x64,1,0x05, 0x74,1,0x05, 0x65,1,0x02, 0x75,1,0x02, 0x67,1,0x23, 0x77,1,0x23,
    0x69,1,0x08, 0x79,1,0x08, 0x6a,1,0x13, 0x7a,1,0x13, 0x6b,1,0x13, 0x7b,1,0x13,
    0x6f,1,0x00, 0x7f,1,0x00, 0x50,1,0x00, 0x52,1,0xd6, 0x53,1,0x08, 0x54,1,0x08,
    0x55,1,0x1e, 0x56,1,0x1c,
    0xa0,3,0x2b,0x24,0x00,
    0xa1,1,0x87, 0xa2,1,0x86, 0xa5,1,0x00, 0xa6,1,0x00, 0xa7,1,0x00, 0xa8,1,0x36,
    0xa9,1,0x7e, 0xaa,1,0x7e, 0xB9,1,0x85, 0xBA,1,0x84, 0xBB,1,0x83, 0xBC,1,0x82,
    0xBD,1,0x81, 0xBE,1,0x80, 0xBF,1,0x01, 0xC0,1,0x02, 0xc1,1,0x00, 0xc2,1,0x00,
    0xc3,1,0x00, 0xc4,1,0x33, 0xc5,1,0x7e, 0xc6,1,0x7e, 0xC8,2,0x33,0x33,
    0xC9,1,0x68, 0xCA,1,0x69, 0xCB,1,0x6a, 0xCC,1,0x6b, 0xCD,2,0x33,0x33,
    0xCE,1,0x6c, 0xCF,1,0x6d, 0xD0,1,0x6e, 0xD1,1,0x6f,
    0xAB,2,0x03,0x67, 0xAC,2,0x03,0x6b, 0xAD,2,0x03,0x68, 0xAE,2,0x03,0x6c,
    0xb3,1,0x00, 0xb4,1,0x00, 0xb5,1,0x00, 0xB6,1,0x32, 0xB7,1,0x7e, 0xB8,1,0x7e,
    0xe0,1,0x00, 0xe1,2,0x03,0x0f, 0xe2,1,0x04, 0xe3,1,0x01, 0xe4,1,0x0e, 0xe5,1,0x01,
    0xe6,1,0x19, 0xe7,1,0x10, 0xe8,1,0x10, 0xea,1,0x12, 0xeb,1,0xd0, 0xec,1,0x04,
    0xed,1,0x07, 0xee,1,0x07, 0xef,1,0x09, 0xf0,1,0xd0, 0xf1,1,0x0e, 0xF9,1,0x17,
    0xf2,4,0x2c,0x1b,0x0b,0x20,
    0xe9,1,0x29, 0xec,1,0x04, 0x35,1,0x00, 0x44,2,0x00,0x10, 0x46,1,0x10,
};

/* ESP-IDF's "panel IO" handle: sends a command byte with DC low, then its
 * parameter or pixel bytes with DC high, over the SPI bus. */
static esp_lcd_panel_io_handle_t io;

/* Sends one command with n parameter bytes (d may be NULL when n is 0). */
static void cmd(uint8_t c, const uint8_t *d, size_t n)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, c, d, n));
}

/* Sends every command in a list in the init1 format described above. */
static void send_list(const uint8_t *l, size_t len)
{
    for (size_t i = 0; i < len; ) {
        uint8_t c = l[i], n = l[i + 1];
        cmd(c, &l[i + 2], n);
        i += 2 + n;         /* skip the command, the count and its parameters */
    }
}

/* Fills the whole controller memory with one colour, a strip at a time. */
static void fill(uint16_t *buf, uint16_t rgb565, const char *name)
{
    /* The panel expects each pixel's high byte first; the ESP32 stores the
     * low byte first. Swap the two bytes once, here. */
    uint16_t be = (rgb565 >> 8) | (rgb565 << 8);
    for (int i = 0; i < GRAM_W * LINES; i++) buf[i] = be;
    /* Drawing window: columns 0..167 (0x2A) and rows 0..427 (0x2B). Each is a
     * start and an end, as 16-bit values sent high byte first. */
    uint8_t ca[] = {0, 0, 0, GRAM_W - 1};
    uint8_t ra[] = {0, 0, (GRAM_H - 1) >> 8, (GRAM_H - 1) & 0xff};
    cmd(0x2a, ca, 4);
    cmd(0x2b, ra, 4);
    for (int y = 0; y < GRAM_H; y += LINES) {
        int h = (GRAM_H - y) < LINES ? (GRAM_H - y) : LINES;   /* last strip may be shorter */
        /* 0x2C starts writing at the window's top-left; 0x3C continues from
         * where the previous write stopped */
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io, y == 0 ? 0x2c : 0x3c, buf, GRAM_W * h * 2));
    }
    printf("DISP fill %s\n", name);
}

void app_main(void)
{
    /* RST and BL are plain digital outputs, not part of SPI */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_RST) | (1ULL << PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_BL, 0);

    /* SPI controller 2. The panel only receives, so there is no MISO line
     * (-1); the quad-SPI pins are unused. max_transfer_sz is the largest
     * single transfer, one strip of pixels. DMA lets the SPI hardware read
     * the pixels from RAM without the CPU copying each byte. */
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = GRAM_W * LINES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    /* 10 MHz is deliberately slow: long breadboard wires distort fast edges.
     * SPI mode 0 is the clock polarity and phase the NV3007 expects. Commands
     * and parameters are 8 bits each. */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS, .dc_gpio_num = PIN_DC,
        .spi_mode = 0, .pclk_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 4, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));

    /* Backlight test first: it only needs VDD, GND and BL, so it isolates
     * power wiring from data wiring */
    printf("DISP backlight OFF for 3 s\n");
    vTaskDelay(pdMS_TO_TICKS(3000));
    gpio_set_level(PIN_BL, 1);
    printf("DISP backlight ON\n");

    /* Hardware reset: hold RST low, release, then give the controller time
     * to start before sending commands */
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    cmd(0xff, (const uint8_t[]){0xa5}, 1);   /* unlock vendor registers */
    send_list(init1, sizeof(init1));
    cmd(0xff, (const uint8_t[]){0x00}, 1);   /* lock them again */
    cmd(0x3a, (const uint8_t[]){0x05}, 1);   /* pixel format: RGB565 */
    cmd(0x11, NULL, 0);                       /* sleep out: panel powers up */
    vTaskDelay(pdMS_TO_TICKS(120));           /* required wait after sleep out */
    cmd(0x29, NULL, 0);                       /* display on */
    vTaskDelay(pdMS_TO_TICKS(20));
    printf("DISP init done\n");

    /* The strip buffer must be in DMA-capable internal RAM, not PSRAM */
    uint16_t *buf = heap_caps_malloc(GRAM_W * LINES * 2, MALLOC_CAP_DMA);
    if (!buf) {
        printf("DISP out of memory\n");
        return;
    }
    /* RGB565 values: red = top 5 bits, green = middle 6, blue = bottom 5 */
    const struct { uint16_t c; const char *n; } seq[] = {
        {0xF800, "RED"}, {0x07E0, "GREEN"}, {0x001F, "BLUE"}, {0xFFFF, "WHITE"}, {0x0000, "BLACK"},
    };
    for (;;) {
        for (int i = 0; i < 5; i++) {
            fill(buf, seq[i].c, seq[i].n);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}
