#include "display_m5tab5.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"

#define TAB5_NATIVE_WIDTH  720U
#define TAB5_NATIVE_HEIGHT 1280U
#define TAB5_LOGICAL_WIDTH  1280U
#define TAB5_LOGICAL_HEIGHT 720U

#define TAB5_RGB565_BLACK 0x0000U
#define TAB5_RGB565_WHITE 0xffffU

static const char *TAG = "display_m5tab5";
static display_m5tab5_t *active_display = NULL;

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    display_m5tab5_t driver;
    solar_os_board_display_t display;
} tab5_display_device_t;

static tab5_display_device_t *device = NULL;

static inline size_t tab5_map_pixel(uint16_t x, uint16_t y)
{
    /* 90-degree clockwise rotation:
     * logical x (0..1279), logical y (0..719) ->
     * native x = y (0..719), native y = 1279 - x (0..1279) */
    const uint16_t nx = y;
    const uint16_t ny = (uint16_t)((TAB5_LOGICAL_WIDTH - 1U) - x);
    return (size_t)ny * TAB5_NATIVE_WIDTH + nx;
}

static uint16_t rgb888_to_rgb565(uint32_t rgb888)
{
    return (uint16_t)(((rgb888 >> 8) & 0xf800U) |
                      ((rgb888 >> 5) & 0x07e0U) |
                      ((rgb888 >> 3) & 0x001fU));
}

static const u8x8_display_info_t tab5_display_info_template = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 0,
    .pre_chip_disable_wait_ns = 0,
    .reset_pulse_width_ms = 20,
    .post_reset_wait_ms = 120,
    .sda_setup_time_ns = 0,
    .sck_pulse_width_ns = 0,
    .sck_clock_hz = 0,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 0,
    .data_setup_time_ns = 0,
    .write_pulse_width_ns = 0,
    .tile_width = 160,
    .tile_height = 90,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = TAB5_LOGICAL_WIDTH,
    .pixel_height = TAB5_LOGICAL_HEIGHT,
};

static uint8_t m5tab5_u8x8_byte_cb(u8x8_t *u8x8, uint8_t message,
                                   uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)message;
    (void)arg_int;
    (void)arg_ptr;
    return 1;
}

void display_m5tab5_invalidate_shadow(display_m5tab5_t *display)
{
    if (display != NULL) {
        display->shadow_valid = false;
    }
}

static esp_err_t m5tab5_draw_tile(display_m5tab5_t *display, const u8x8_tile_t *tile)
{
    if (display == NULL || tile == NULL || tile->tile_ptr == NULL ||
        display->framebuffer == NULL || tile->cnt == 0) {
        return ESP_OK;
    }

    const uint16_t fg = display->foreground_rgb565;
    const uint16_t bg = display->background_rgb565;
    const uint8_t tile_x = tile->x_pos;
    const uint8_t tile_y = tile->y_pos;
    const uint8_t count = tile->cnt;

    for (uint8_t t = 0; t < count; t++) {
        const uint16_t curr_tile_x = (uint16_t)(tile_x + t);
        if (curr_tile_x >= display->tile_width || tile_y >= display->tile_height) {
            continue;
        }

        const size_t tile_idx = ((size_t)tile_y * display->tile_width + curr_tile_x) * 8U;
        const uint8_t *src = tile->tile_ptr + (size_t)t * 8U;

        if (display->shadow_valid && display->shadow != NULL &&
            memcmp(display->shadow + tile_idx, src, 8U) == 0) {
            continue;
        }
        if (display->shadow != NULL) {
            memcpy(display->shadow + tile_idx, src, 8U);
        }

        const uint16_t base_x = (uint16_t)(curr_tile_x * 8U);
        const uint16_t base_y = (uint16_t)(tile_y * 8U);

        for (uint8_t px = 0; px < 8; px++) {
            const uint16_t lx = (uint16_t)(base_x + px);
            if (lx >= display->config.width) {
                break;
            }
            const uint8_t col_bits = src[px];

            for (uint8_t py = 0; py < 8; py++) {
                const uint16_t ly = (uint16_t)(base_y + py);
                if (ly >= display->config.height) {
                    break;
                }

                const uint16_t color = (col_bits & (1U << py)) ? fg : bg;
                const size_t pixel_idx = tab5_map_pixel(lx, ly);
                display->framebuffer[pixel_idx] = color;
            }
        }
    }

    return ESP_OK;
}

static uint8_t m5tab5_u8x8_display_cb(u8x8_t *u8x8, uint8_t message,
                                      uint8_t arg_int, void *arg_ptr)
{
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        if (active_display != NULL) {
            u8x8_d_helper_display_setup_memory(u8x8, &active_display->display_info);
            return 1;
        }
        return 0;
    }

    display_m5tab5_t *display = active_display;
    if (display == NULL) {
        return 0;
    }

    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        return 1;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        display_m5tab5_set_backlight(display, arg_int == 0 ? display->backlight_percent : 0);
        return 1;
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        return m5tab5_draw_tile(display, (const u8x8_tile_t *)arg_ptr) == ESP_OK ? 1 : 0;
    case U8X8_MSG_DISPLAY_REFRESH:
        if (display->panel != NULL && display->framebuffer != NULL) {
            esp_lcd_panel_draw_bitmap(display->panel, 0, 0, TAB5_NATIVE_WIDTH, TAB5_NATIVE_HEIGHT, display->framebuffer);
        }
        display->shadow_valid = true;
        return 1;
    default:
        return 0;
    }
}

esp_err_t display_m5tab5_init(display_m5tab5_t *display, const display_m5tab5_config_t *config)
{
    if (display == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->config = *config;
    if (display->config.width == 0) {
        display->config.width = TAB5_LOGICAL_WIDTH;
    }
    if (display->config.height == 0) {
        display->config.height = TAB5_LOGICAL_HEIGHT;
    }
    display->tile_width = (display->config.width + 7U) / 8U;
    display->tile_height = (display->config.height + 7U) / 8U;
    display->buffer_size = (size_t)display->tile_width * display->tile_height * 8U;
    display->shadow_size = display->buffer_size;
    display->display_info = tab5_display_info_template;
    display->display_info.tile_width = display->tile_width;
    display->display_info.tile_height = display->tile_height;
    display->display_info.pixel_width = display->config.width;
    display->display_info.pixel_height = display->config.height;
    display->foreground_rgb565 = TAB5_RGB565_WHITE;
    display->background_rgb565 = TAB5_RGB565_BLACK;
    display->backlight_percent = 100;
    display->shadow_valid = false;

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init failed");
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    bsp_set_charge_qc_en(true);
    bsp_set_charge_en(true);
    bsp_set_ext_5v_en(true);
    bsp_set_usb_5v_en(true);
    bsp_set_wifi_power_enable(true);
    bsp_set_ext_antenna_enable(false);
    static const gpio_num_t s_sdio_gpios[] = {
        GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_15
    };
    for (size_t i = 0; i < sizeof(s_sdio_gpios) / sizeof(s_sdio_gpios[0]); i++) {
        gpio_set_drive_capability(s_sdio_gpios[i], GPIO_DRIVE_CAP_0);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    bsp_reset_tp();
    bsp_i2c_scan();

    bsp_lcd_handles_t lcd_handles = {0};
    bsp_display_type_t display_type = bsp_detect_display_type();
    if (display_type == BSP_DISPLAY_TYPE_ST7123 || display_type == BSP_DISPLAY_TYPE_ST7121) {
        ESP_RETURN_ON_ERROR(bsp_display_new_with_handles_to_st7123(NULL, &lcd_handles),
                            TAG, "new ST7123 LCD failed");
    } else {
        ESP_RETURN_ON_ERROR(bsp_display_new_with_handles(NULL, &lcd_handles),
                            TAG, "new ILI9881C LCD failed");
    }

    display->panel = lcd_handles.panel;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(display->panel, true), TAG, "panel on failed");

    /* Acquire continuous DPI scanout frame buffer */
    esp_err_t fb_err = esp_lcd_dpi_panel_get_frame_buffer(display->panel, 1,
                                                          (void **)&display->framebuffer);
    if (fb_err != ESP_OK || display->framebuffer == NULL) {
        ESP_LOGW(TAG, "Direct DPI framebuffer unavailable (%s), allocating PSRAM buffer",
                 esp_err_to_name(fb_err));
        display->framebuffer = heap_caps_calloc(TAB5_NATIVE_WIDTH * TAB5_NATIVE_HEIGHT,
                                               sizeof(uint16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (display->framebuffer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    display->buffer = heap_caps_calloc(1, display->buffer_size,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    display->shadow = heap_caps_calloc(1, display->shadow_size,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display->buffer == NULL || display->shadow == NULL) {
        display_m5tab5_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    active_display = display;
    u8g2_SetupDisplay(&display->u8g2, m5tab5_u8x8_display_cb, u8x8_dummy_cb,
                      m5tab5_u8x8_byte_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2, display->buffer, display->tile_height,
                     u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);

    display_m5tab5_set_backlight(display, 100);
    if (display->panel != NULL && display->framebuffer != NULL) {
        memset(display->framebuffer, 0, TAB5_NATIVE_WIDTH * TAB5_NATIVE_HEIGHT * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(display->panel, 0, 0, TAB5_NATIVE_WIDTH, TAB5_NATIVE_HEIGHT, display->framebuffer);
    }
    ESP_LOGI(TAG, "M5Stack Tab5 MIPI DSI display initialized (%ux%u landscape)",
             display->config.width, display->config.height);

    return ESP_OK;
}

esp_err_t display_m5tab5_resume(display_m5tab5_t *display)
{
    if (display == NULL || display->panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    display_m5tab5_invalidate_shadow(display);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(display->panel, true), TAG, "resume failed");
    return display_m5tab5_set_backlight(display, display->backlight_percent);
}

void display_m5tab5_deinit(display_m5tab5_t *display)
{
    if (display == NULL) {
        return;
    }
    display_m5tab5_set_backlight(display, 0);
    if (display->panel != NULL) {
        esp_lcd_panel_disp_on_off(display->panel, false);
    }
    if (display->buffer != NULL) {
        heap_caps_free(display->buffer);
        display->buffer = NULL;
    }
    if (display->shadow != NULL) {
        heap_caps_free(display->shadow);
        display->shadow = NULL;
    }
    if (active_display == display) {
        active_display = NULL;
    }
}

u8g2_t *display_m5tab5_get_u8g2(display_m5tab5_t *display)
{
    return display != NULL ? &display->u8g2 : NULL;
}

bool display_m5tab5_backlight_supported(void)
{
    return true;
}

esp_err_t display_m5tab5_get_backlight(const display_m5tab5_t *display, uint8_t *percent)
{
    if (display == NULL || percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *percent = display->backlight_percent;
    return ESP_OK;
}

esp_err_t display_m5tab5_set_backlight(display_m5tab5_t *display, uint8_t percent)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100) {
        percent = 100;
    }
    display->backlight_percent = percent;
    return bsp_display_brightness_set((int)percent);
}

esp_err_t display_m5tab5_set_colors(display_m5tab5_t *display,
                                    uint32_t foreground_rgb888,
                                    uint32_t background_rgb888)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    display->foreground_rgb565 = rgb888_to_rgb565(foreground_rgb888);
    display->background_rgb565 = rgb888_to_rgb565(background_rgb888);
    display_m5tab5_invalidate_shadow(display);
    return ESP_OK;
}

esp_err_t display_m5tab5_present_surface(display_m5tab5_t *display,
                                        const solar_os_display_surface_t *surface)
{
    if (display == NULL || surface == NULL || display->framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t *palette = surface->palette_rgb565;
    const uint8_t *data = surface->data;
    const uint16_t w = surface->width < display->config.width ? surface->width : display->config.width;
    const uint16_t h = surface->height < display->config.height ? surface->height : display->config.height;

    for (uint16_t y = 0; y < h; y++) {
        const size_t row_offset = (size_t)y * surface->stride;
        for (uint16_t x = 0; x < w; x++) {
            const uint8_t idx = data[row_offset + x];
            const uint16_t color = palette != NULL ? palette[idx] : (idx ? display->foreground_rgb565 : display->background_rgb565);
            display->framebuffer[tab5_map_pixel(x, y)] = color;
        }
    }

    display_m5tab5_invalidate_shadow(display);

    if (display->panel != NULL && display->framebuffer != NULL) {
        esp_lcd_panel_draw_bitmap(display->panel, 0, 0, TAB5_NATIVE_WIDTH, TAB5_NATIVE_HEIGHT, display->framebuffer);
    }

    return ESP_OK;
}

esp_err_t display_m5tab5_present_frame(display_m5tab5_t *display,
                                      const solar_os_display_raster_t *frame)
{
    if (display == NULL || frame == NULL || display->framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t *palette = frame->palette_rgb565;
    const uint8_t *data = frame->data;
    const uint16_t x_start = frame->x;
    const uint16_t y_start = frame->y;
    const uint16_t w = frame->width;
    const uint16_t h = frame->height;

    if (frame->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8) {
        for (uint16_t row = 0; row < h; row++) {
            const uint16_t ly = (uint16_t)(y_start + row);
            if (ly >= display->config.height) break;
            const size_t row_off = (size_t)row * frame->source_stride;
            for (uint16_t col = 0; col < w; col++) {
                const uint16_t lx = (uint16_t)(x_start + col);
                if (lx >= display->config.width) break;
                const uint8_t idx = data[row_off + col];
                const uint16_t color = palette ? palette[idx] : 0;
                display->framebuffer[tab5_map_pixel(lx, ly)] = color;
            }
        }
    } else if (frame->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2) {
        for (uint16_t row = 0; row < h; row++) {
            const uint16_t ly = (uint16_t)(y_start + row);
            if (ly >= display->config.height) break;
            const size_t row_off = (size_t)row * frame->source_stride;
            for (uint16_t col = 0; col < w; col++) {
                const uint16_t lx = (uint16_t)(x_start + col);
                if (lx >= display->config.width) break;
                const uint8_t byte_val = data[row_off + (col >> 2U)];
                const uint8_t idx = (byte_val >> ((col & 3U) * 2U)) & 0x03U;
                const uint16_t color = palette ? palette[idx] : 0;
                display->framebuffer[tab5_map_pixel(lx, ly)] = color;
            }
        }
    }

    display_m5tab5_invalidate_shadow(display);

    if (display->panel != NULL && display->framebuffer != NULL) {
        esp_lcd_panel_draw_bitmap(display->panel, 0, 0, TAB5_NATIVE_WIDTH, TAB5_NATIVE_HEIGHT, display->framebuffer);
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Expansion Driver Registration                                              */
/* -------------------------------------------------------------------------- */

static esp_err_t runtime_ready(solar_os_board_display_t *disp)
{
    return disp != NULL && disp->driver != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *disp)
{
    return disp != NULL && disp->driver != NULL ?
        display_m5tab5_resume((display_m5tab5_t *)disp->driver) : ESP_ERR_INVALID_STATE;
}

static void deinit(solar_os_board_display_t *disp)
{
    if (disp != NULL && disp->driver != NULL) {
        display_m5tab5_deinit((display_m5tab5_t *)disp->driver);
        disp->ready = false;
    }
}

static bool brightness_supported(const solar_os_board_display_t *disp)
{
    (void)disp;
    return display_m5tab5_backlight_supported();
}

static esp_err_t get_brightness(const solar_os_board_display_t *disp, uint8_t *percent)
{
    return disp != NULL ? display_m5tab5_get_backlight((display_m5tab5_t *)disp->driver, percent) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t set_brightness(solar_os_board_display_t *disp, uint8_t percent)
{
    return disp != NULL ? display_m5tab5_set_backlight((display_m5tab5_t *)disp->driver, percent) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t set_colors(solar_os_board_display_t *disp, uint32_t fg, uint32_t bg)
{
    return disp != NULL ? display_m5tab5_set_colors((display_m5tab5_t *)disp->driver, fg, bg) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_surface(solar_os_board_display_t *disp, const solar_os_display_surface_t *surface)
{
    return disp != NULL ? display_m5tab5_present_surface((display_m5tab5_t *)disp->driver, surface) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_frame(solar_os_board_display_t *disp, const solar_os_display_raster_t *frame)
{
    return disp != NULL ? display_m5tab5_present_frame((display_m5tab5_t *)disp->driver, frame) :
        ESP_ERR_INVALID_STATE;
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = runtime_ready,
    .resume = resume,
    .deinit = deinit,
    .brightness_supported = brightness_supported,
    .get_brightness = get_brightness,
    .set_brightness = set_brightness,
    .set_colors = set_colors,
    .present_surface = present_surface,
    .present_frame = present_frame,
};

static esp_err_t attach_tab5_display(const char *name,
                                     const solar_os_expansion_binding_t *bindings,
                                     size_t binding_count)
{
    (void)bindings;
    (void)binding_count;

    if (device != NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    display_m5tab5_config_t config = {
        .backlight_pin = 22,
        .backlight_active_high = true,
        .backlight_pwm = true,
        .backlight_pwm_hz = 20000,
        .width = TAB5_LOGICAL_WIDTH,
        .height = TAB5_LOGICAL_HEIGHT,
    };

    esp_err_t ret = display_m5tab5_init(&device->driver, &config);
    if (ret != ESP_OK) {
        heap_caps_free(device);
        device = NULL;
        return ret;
    }

    strlcpy(device->name, name, sizeof(device->name));
    u8g2_t *const u8g2 = display_m5tab5_get_u8g2(&device->driver);

    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = "m5tab5",
        .u8g2 = u8g2,
        .controller = "ST7123",
        .width = TAB5_LOGICAL_WIDTH,
        .height = TAB5_LOGICAL_HEIGHT,
        .surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT,
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT,
        .preferred_stream_fps = 30,
        .max_stream_pixels_per_second = 30000000U,
        .ready = true,
    };

    device->primary = strcmp(name, SOLAR_OS_DISPLAY_PRIMARY_TARGET) == 0;
    ret = solar_os_board_display_register_primary(&device->display);
    if (ret != ESP_OK) {
        display_m5tab5_deinit(&device->driver);
        heap_caps_free(device);
        device = NULL;
        return ret;
    }

    device->active = true;
    return ESP_OK;
}

static esp_err_t detach_tab5_display(const char *name)
{
    if (device == NULL || !device->active || name == NULL || strcmp(device->name, name) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_board_display_unregister_primary(&device->display);
    display_m5tab5_deinit(&device->driver);
    heap_caps_free(device);
    device = NULL;
    return ESP_OK;
}

static const int bool_values[] = {0, 1};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "bl", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bl"},
    {.key = "active", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .allowed_values = bool_values, .allowed_value_count = 2},
    {.key = "pwm", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "pwm", .allowed_values = bool_values, .allowed_value_count = 2},
};

const solar_os_expansion_driver_t solar_os_m5tab5_expansion_driver = {
    .name = "m5tab5",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "M5Stack Tab5 MIPI DSI display",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach_tab5_display,
    .detach = detach_tab5_display,
};
