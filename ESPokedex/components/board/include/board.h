#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pin Definitions for Waveshare ESP32-C6-LCD-1.47
#define BOARD_LCD_MOSI       GPIO_NUM_6
#define BOARD_LCD_SCLK       GPIO_NUM_7
#define BOARD_LCD_CS         GPIO_NUM_14
#define BOARD_LCD_DC         GPIO_NUM_15
#define BOARD_LCD_RST        GPIO_NUM_21
#define BOARD_LCD_BL         GPIO_NUM_22

#define BOARD_SD_CS          GPIO_NUM_4
#define BOARD_SD_MISO        GPIO_NUM_5

#define BOARD_RGB_LED        GPIO_NUM_8
#define BOARD_BTN_BOOT       GPIO_NUM_9

// Display Geometry (Portrait: 172 x 320)
#define BOARD_LCD_WIDTH      172
#define BOARD_LCD_HEIGHT     320

// ST7789 RAM Offset for 172x320 centered in 240x320 panel
#define BOARD_LCD_X_OFFSET   34
#define BOARD_LCD_Y_OFFSET   0

// Standard 16-bit RGB565 Colors (Big-Endian for SPI transmission)
#define COLOR_BLACK          0x0000
#define COLOR_WHITE          0xFFFF
#define COLOR_RED            0xF800
#define COLOR_GREEN          0x07E0
#define COLOR_BLUE           0x001F
#define COLOR_YELLOW         0xFFE0
#define COLOR_CYAN           0x07FF
#define COLOR_MAGENTA        0xF81F

/**
 * @brief Initialize board peripherals (SPI bus, ST7789 display, backlight LEDC)
 */
esp_err_t board_init(void);

/**
 * @brief Set LCD backlight brightness percentage (0 - 90)
 */
void board_lcd_set_backlight(uint8_t percent);

/**
 * @brief Draw a bitmap area to the LCD
 * @param x_start X start coordinate [0 .. BOARD_LCD_WIDTH - 1]
 * @param y_start Y start coordinate [0 .. BOARD_LCD_HEIGHT - 1]
 * @param x_end X end coordinate (exclusive) [1 .. BOARD_LCD_WIDTH]
 * @param y_end Y end coordinate (exclusive) [1 .. BOARD_LCD_HEIGHT]
 * @param color_data Buffer of uint16_t RGB565 pixel values
 */
esp_err_t board_lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data);

/**
 * @brief Fill entire LCD with a solid color
 */
esp_err_t board_lcd_fill_screen(uint16_t color);

/**
 * @brief Display color calibration & test pattern
 */
void board_lcd_draw_test_pattern(void);

/**
 * @brief Initialize onboard WS2812 RGB LED (GPIO 8)
 */
esp_err_t board_rgb_led_init(void);

/**
 * @brief Set onboard RGB LED color
 */
esp_err_t board_rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Initialize BOOT button (GPIO 9) with internal pull-up
 */
esp_err_t board_button_init(void);

/**
 * @brief Check if BOOT button is currently pressed (active low)
 */
bool board_button_is_pressed(void);

#ifdef __cplusplus
}
#endif

