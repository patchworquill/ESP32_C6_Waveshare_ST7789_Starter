#include <string.h>
#include "board.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_check.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define LCD_HOST                SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ      (20 * 1000 * 1000)
#define LCD_CMD_BITS            8
#define LCD_PARAM_BITS          8

#define BK_LIGHT_LEDC_TIMER     LEDC_TIMER_0
#define BK_LIGHT_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define BK_LIGHT_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BK_LIGHT_LEDC_DUTY_RES  LEDC_TIMER_13_BIT
#define BK_LIGHT_LEDC_MAX_DUTY  ((1 << BK_LIGHT_LEDC_DUTY_RES) - 1)

#define RMT_LED_RESOLUTION_HZ   10000000 // 10MHz resolution (0.1us per tick)

static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;

/* ---------------- WS2812 RMT Encoder Implementation ---------------- */

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_ws2812_encoder_t;

static size_t rmt_encode_ws2812(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state)
{
    rmt_ws2812_encoder_t *led_encoder = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = led_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder = led_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (led_encoder->state) {
    case 0:
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        /* fall-through */
    case 1:
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_encoder->reset_code,
                                                sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_ws2812_encoder(rmt_encoder_t *encoder)
{
    rmt_ws2812_encoder_t *led_encoder = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t rmt_ws2812_encoder_reset(rmt_encoder_t *encoder)
{
    rmt_ws2812_encoder_t *led_encoder = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t rmt_new_ws2812_encoder(rmt_encoder_handle_t *ret_encoder)
{
    rmt_ws2812_encoder_t *led_encoder = rmt_alloc_encoder_mem(sizeof(rmt_ws2812_encoder_t));
    if (!led_encoder) {
        return ESP_ERR_NO_MEM;
    }
    led_encoder->base.encode = rmt_encode_ws2812;
    led_encoder->base.del = rmt_del_ws2812_encoder;
    led_encoder->base.reset = rmt_ws2812_encoder_reset;
    led_encoder->state = 0;

    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = (uint32_t)(0.3 * RMT_LED_RESOLUTION_HZ / 1000000), // 3 ticks
            .level1 = 0,
            .duration1 = (uint32_t)(0.9 * RMT_LED_RESOLUTION_HZ / 1000000), // 9 ticks
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = (uint32_t)(0.9 * RMT_LED_RESOLUTION_HZ / 1000000), // 9 ticks
            .level1 = 0,
            .duration1 = (uint32_t)(0.3 * RMT_LED_RESOLUTION_HZ / 1000000), // 3 ticks
        },
        .flags.msb_first = 1
    };
    esp_err_t ret = rmt_new_bytes_encoder(&bytes_config, &led_encoder->bytes_encoder);
    if (ret != ESP_OK) {
        free(led_encoder);
        return ret;
    }

    rmt_copy_encoder_config_t copy_config = {};
    ret = rmt_new_copy_encoder(&copy_config, &led_encoder->copy_encoder);
    if (ret != ESP_OK) {
        rmt_del_encoder(led_encoder->bytes_encoder);
        free(led_encoder);
        return ret;
    }

    uint32_t reset_ticks = (uint32_t)(RMT_LED_RESOLUTION_HZ / 1000000 * 50 / 2);
    led_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };
    *ret_encoder = &led_encoder->base;
    return ESP_OK;
}


static void board_lcd_reset(void)
{
    gpio_config_t rst_conf = {
        .pin_bit_mask = (1ULL << BOARD_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_conf);
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(15));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void board_backlight_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = BK_LIGHT_LEDC_MODE,
        .timer_num = BK_LIGHT_LEDC_TIMER,
        .duty_resolution = BK_LIGHT_LEDC_DUTY_RES,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = BK_LIGHT_LEDC_MODE,
        .channel = BK_LIGHT_LEDC_CHANNEL,
        .timer_sel = BK_LIGHT_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BOARD_LCD_BL,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

void board_lcd_set_backlight(uint8_t percent)
{
    if (percent > 90) { // limit to 90% to avoid damage to the display
        percent = 90;
    }
    uint32_t duty = (BK_LIGHT_LEDC_MAX_DUTY * percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(BK_LIGHT_LEDC_MODE, BK_LIGHT_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(BK_LIGHT_LEDC_MODE, BK_LIGHT_LEDC_CHANNEL));
}

static void st7789_init_commands(esp_lcd_panel_io_handle_t io)
{
    // Sleep Out
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(120));

    // Memory Access Control: 0x00 for Portrait RGB, 0x08 for Portrait BGR
    uint8_t madctl = 0x00;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x36, &madctl, 1));

    // Interface Pixel Format: 16-bit / 65k RGB (RGB565)
    uint8_t colmod = 0x55;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x3A, &colmod, 1));

    // Porch Setting
    uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB2, porch, sizeof(porch)));

    // Gate Control
    uint8_t gate = 0x75;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB7, &gate, 1));

    // VCOM Setting
    uint8_t vcom = 0x1A;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xBB, &vcom, 1));

    // LCM Control
    uint8_t lcm = 0x80;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC0, &lcm, 1));

    // VDV and VRH Command Enable
    uint8_t vdv_vrh_enable[] = {0x01, 0xFF};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC2, vdv_vrh_enable, sizeof(vdv_vrh_enable)));

    // VRH Set
    uint8_t vrh = 0x13;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC3, &vrh, 1));

    // VDV Set
    uint8_t vdv = 0x20;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC4, &vdv, 1));

    // Frame Rate Control
    uint8_t fr = 0x0F;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC6, &fr, 1));

    // Power Control 1
    uint8_t pwr1[] = {0xA4, 0xA1};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xD0, pwr1, sizeof(pwr1)));

    // Positive Voltage Gamma Control
    uint8_t pv_gamma[] = {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xE0, pv_gamma, sizeof(pv_gamma)));

    // Negative Voltage Gamma Control
    uint8_t nv_gamma[] = {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xE1, nv_gamma, sizeof(nv_gamma)));

    // Display Inversion ON (Required for ST7789 IPS)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x21, NULL, 0));

    // Display ON
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
}

esp_err_t board_rgb_led_init(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = BOARD_RGB_LED,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_LED_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_chan_config, &s_led_chan), TAG, "RMT new tx channel failed");
    ESP_RETURN_ON_ERROR(rmt_new_ws2812_encoder(&s_led_encoder), TAG, "WS2812 encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_chan), TAG, "RMT enable failed");
    ESP_LOGI(TAG, "RGB LED initialized on GPIO %d", BOARD_RGB_LED);
    return board_rgb_led_set_color(0, 0, 0);
}

esp_err_t board_rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_led_chan || !s_led_encoder) {
        return ESP_ERR_INVALID_STATE;
    }
    // WS2812 GRB ordering
    uint8_t grb[3] = {green, red, blue};
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_transmit(s_led_chan, s_led_encoder, grb, sizeof(grb), &tx_config), TAG, "RMT transmit failed");
    return rmt_tx_wait_all_done(s_led_chan, portMAX_DELAY);
}

esp_err_t board_button_init(void)
{
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BOARD_BTN_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_LOGI(TAG, "BOOT button initialized on GPIO %d", BOARD_BTN_BOOT);
    return gpio_config(&btn_conf);
}

bool board_button_is_pressed(void)
{
    return gpio_get_level(BOARD_BTN_BOOT) == 0;
}

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "Initializing Waveshare ESP32-C6 LCD board");

    board_backlight_init();
    board_lcd_set_backlight(0);

    spi_bus_config_t buscfg = {
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_WIDTH * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_io_handle));

    board_lcd_reset();
    st7789_init_commands(s_io_handle);

    // Initialize RGB LED
    ESP_ERROR_CHECK(board_rgb_led_init());

    // Initialize BOOT button
    ESP_ERROR_CHECK(board_button_init());

    // Set backlight to 50% to avoid heating
    board_lcd_set_backlight(50);
    ESP_LOGI(TAG, "Board peripherals initialized successfully");
    return ESP_OK;
}


esp_err_t board_lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    if (!s_io_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    int xs = x_start + BOARD_LCD_X_OFFSET;
    int xe = x_end + BOARD_LCD_X_OFFSET - 1;
    int ys = y_start + BOARD_LCD_Y_OFFSET;
    int ye = y_end + BOARD_LCD_Y_OFFSET - 1;

    // Column Address Set (0x2A)
    uint8_t caset[] = {
        (uint8_t)((xs >> 8) & 0xFF),
        (uint8_t)(xs & 0xFF),
        (uint8_t)((xe >> 8) & 0xFF),
        (uint8_t)(xe & 0xFF)
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io_handle, 0x2A, caset, sizeof(caset)));

    // Row Address Set (0x2B)
    uint8_t raset[] = {
        (uint8_t)((ys >> 8) & 0xFF),
        (uint8_t)(ys & 0xFF),
        (uint8_t)((ye >> 8) & 0xFF),
        (uint8_t)(ye & 0xFF)
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io_handle, 0x2B, raset, sizeof(raset)));

    // Memory Write (0x2C)
    size_t len = (x_end - x_start) * (y_end - y_start) * sizeof(uint16_t);
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_io_handle, 0x2C, color_data, len));

    return ESP_OK;
}

esp_err_t board_lcd_fill_screen(uint16_t color)
{
    const int CHUNK_LINES = 20;
    size_t chunk_pixels = BOARD_LCD_WIDTH * CHUNK_LINES;
    uint16_t *buf = heap_caps_malloc(chunk_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    // Convert to Big-Endian byte order for SPI transmission
    uint16_t be_color = (color >> 8) | (color << 8);
    for (size_t i = 0; i < chunk_pixels; i++) {
        buf[i] = be_color;
    }

    for (int y = 0; y < BOARD_LCD_HEIGHT; y += CHUNK_LINES) {
        int y_end = y + CHUNK_LINES;
        if (y_end > BOARD_LCD_HEIGHT) {
            y_end = BOARD_LCD_HEIGHT;
        }
        board_lcd_draw_bitmap(0, y, BOARD_LCD_WIDTH, y_end, buf);
    }

    free(buf);
    return ESP_OK;
}

void board_lcd_draw_test_pattern(void)
{
    ESP_LOGI(TAG, "Drawing color calibration test pattern (172x320)");

    // 8 Color Bars across the screen height:
    // Red, Green, Blue, Yellow, Cyan, Magenta, White, Black
    static const uint16_t test_colors[8] = {
        COLOR_RED,
        COLOR_GREEN,
        COLOR_BLUE,
        COLOR_YELLOW,
        COLOR_CYAN,
        COLOR_MAGENTA,
        COLOR_WHITE,
        COLOR_BLACK
    };

    const int bar_height = BOARD_LCD_HEIGHT / 8; // 40 lines per bar
    uint16_t *buf = heap_caps_malloc(BOARD_LCD_WIDTH * bar_height * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate test pattern buffer");
        return;
    }

    for (int bar = 0; bar < 8; bar++) {
        uint16_t col = test_colors[bar];
        uint16_t be_col = (col >> 8) | (col << 8);

        for (int y = 0; y < bar_height; y++) {
            for (int x = 0; x < BOARD_LCD_WIDTH; x++) {
                // Draw a 2-pixel white border around each color block
                if (x < 2 || x >= BOARD_LCD_WIDTH - 2 || y < 2 || y >= bar_height - 2) {
                    buf[y * BOARD_LCD_WIDTH + x] = 0xFFFF;
                } else {
                    buf[y * BOARD_LCD_WIDTH + x] = be_col;
                }
            }
        }

        int y_start = bar * bar_height;
        board_lcd_draw_bitmap(0, y_start, BOARD_LCD_WIDTH, y_start + bar_height, buf);
    }

    free(buf);
    ESP_LOGI(TAG, "Test pattern drawing complete");
}
