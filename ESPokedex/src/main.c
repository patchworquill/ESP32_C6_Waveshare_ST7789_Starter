#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "board.h"

static const char *TAG = "ESPokedex";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    const char *name;
    uint16_t rgb565;
} led_color_t;

static const led_color_t COLOR_PALETTE[] = {
    {32,   0,   0, "Red",     COLOR_RED},
    { 0,  32,   0, "Green",   COLOR_GREEN},
    { 0,   0,  32, "Blue",    COLOR_BLUE},
    {32,  32,   0, "Yellow",  COLOR_YELLOW},
    {32,   0,  32, "Magenta", COLOR_MAGENTA},
    { 0,  32,  32, "Cyan",    COLOR_CYAN},
    {32,  32,  32, "White",   COLOR_WHITE},
    { 0,   0,   0, "Off",     COLOR_BLACK},
};
#define NUM_COLORS (sizeof(COLOR_PALETTE) / sizeof(COLOR_PALETTE[0]))

void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "   ESPokedex - Waveshare ESP32-C6 LCD   ");
    ESP_LOGI(TAG, "========================================");

    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Chip: ESP32-C6 (rev %d), %d CPU core(s)",
             chip_info.revision, chip_info.cores);
    ESP_LOGI(TAG, "Flash size: %" PRIu32 " MB", flash_size / (1024 * 1024));

    // Initialize board peripherals (LCD, Backlight, RGB LED, Button)
    ESP_ERROR_CHECK(board_init());

    // Display color calibration test pattern
    board_lcd_draw_test_pattern();

    // Start RGB LED on first color (Red)
    size_t color_idx = 0;
    board_rgb_led_set_color(COLOR_PALETTE[color_idx].r,
                            COLOR_PALETTE[color_idx].g,
                            COLOR_PALETTE[color_idx].b);
    ESP_LOGI(TAG, "RGB LED initial color: %s", COLOR_PALETTE[color_idx].name);

    bool prev_button_state = false;
    uint32_t heartbeat_timer = 0;
    int tick = 0;

    ESP_LOGI(TAG, "System ready. Press BOOT button (GPIO 9) to cycle RGB LED colors.");

    while (1) {
        bool btn_pressed = board_button_is_pressed();

        // Button edge detection: released -> pressed
        if (btn_pressed && !prev_button_state) {
            color_idx = (color_idx + 1) % NUM_COLORS;
            board_rgb_led_set_color(COLOR_PALETTE[color_idx].r,
                                    COLOR_PALETTE[color_idx].g,
                                    COLOR_PALETTE[color_idx].b);
            ESP_LOGI(TAG, "Button pressed! Cycled RGB LED to: %s", COLOR_PALETTE[color_idx].name);
        }
        prev_button_state = btn_pressed;

        // Periodic heartbeat every 2000 ms (20 * 100ms)
        if (++heartbeat_timer >= 20) {
            heartbeat_timer = 0;
            ESP_LOGI(TAG, "Heartbeat tick %d | LED: %s | Button: %s",
                     tick++, COLOR_PALETTE[color_idx].name, btn_pressed ? "PRESSED" : "UP");
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms polling/debounce
    }
}


