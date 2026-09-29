/*
 * Pin assignment, verified on hardware during stage 0.
 *
 * The single source of pin numbers for the firmware: drivers take pins from
 * here, never as literal numbers. The same table, with the wiring of each
 * module, is in README.md.
 *
 * Pins that must not be used on this board:
 *   GPIO 26-37   wired to the flash chip and to the PSRAM inside the ESP32 package
 *   GPIO 0, 3, 45, 46   "strapping" pins, read at power-on to choose the boot
 *                mode; a peripheral pulling them the wrong way can stop the
 *                chip from booting
 *   GPIO 19, 20  native USB
 *   GPIO 43, 44  UART0, the serial console
 */
#pragma once

#include "driver/gpio.h"

/* INMP441 microphone, on I2S controller 0. I2S is a three-wire serial audio
 * bus: SCK is the bit clock, WS (word select) marks left or right channel,
 * SD carries the audio data from the microphone to the chip. */
#define BOARD_MIC_SCK       GPIO_NUM_4
#define BOARD_MIC_WS        GPIO_NUM_5
#define BOARD_MIC_SD        GPIO_NUM_6

/* MAX98357A amplifier, on I2S controller 1. BCLK and LRC are the same bit
 * clock and word select as above; DIN carries audio from the chip to the
 * amplifier. SD is not I2S: it is a plain on/off input, low = amplifier off. */
#define BOARD_AMP_BCLK      GPIO_NUM_15
#define BOARD_AMP_LRC       GPIO_NUM_16
#define BOARD_AMP_DIN       GPIO_NUM_7
#define BOARD_AMP_SD        GPIO_NUM_17

/* NV3007 display, on SPI controller 2. SCLK is the clock, MOSI the data from
 * the chip, CS selects the display. DC tells the display whether a byte is a
 * command or pixel data, RST resets it, BL switches the backlight. */
#define BOARD_LCD_SCLK      GPIO_NUM_12
#define BOARD_LCD_MOSI      GPIO_NUM_11
#define BOARD_LCD_CS        GPIO_NUM_10
#define BOARD_LCD_DC        GPIO_NUM_9
#define BOARD_LCD_RST       GPIO_NUM_8
#define BOARD_LCD_BL        GPIO_NUM_14

/* Panel geometry, in the controller's native portrait orientation: 142
 * visible columns by 428 rows. The controller's memory is 168 columns wide,
 * so the visible columns start part-way across it, with 12 unused columns
 * on one side and 14 on the other. Which of the two applies depends on the
 * orientation the display is used in; see display.c. */
#define BOARD_LCD_WIDTH         142
#define BOARD_LCD_HEIGHT        428
