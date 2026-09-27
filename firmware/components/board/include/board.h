/*
 * Pin assignment, verified on hardware during stage 0.
 * The single source of pin numbers for the firmware; see the pinout in
 * README.md. GPIO 26-37 belong to the flash and PSRAM and must not be used.
 */
#pragma once

#include "driver/gpio.h"

/* INMP441 microphone, I2S0 */
#define BOARD_MIC_SCK       GPIO_NUM_4
#define BOARD_MIC_WS        GPIO_NUM_5
#define BOARD_MIC_SD        GPIO_NUM_6

/* MAX98357A amplifier, I2S1 */
#define BOARD_AMP_BCLK      GPIO_NUM_15
#define BOARD_AMP_LRC       GPIO_NUM_16
#define BOARD_AMP_DIN       GPIO_NUM_7
#define BOARD_AMP_SD        GPIO_NUM_17

/* NV3007 display, SPI2 */
#define BOARD_LCD_SCLK      GPIO_NUM_12
#define BOARD_LCD_MOSI      GPIO_NUM_11
#define BOARD_LCD_CS        GPIO_NUM_10
#define BOARD_LCD_DC        GPIO_NUM_9
#define BOARD_LCD_RST       GPIO_NUM_8
#define BOARD_LCD_BL        GPIO_NUM_14
