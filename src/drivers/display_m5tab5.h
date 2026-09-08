#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "solar_os_display_surface.h"
#include "solar_os_expansion.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int backlight_pin;
    bool backlight_active_high;
    bool backlight_pwm;
    uint32_t backlight_pwm_hz;
    uint16_t width;
    uint16_t height;
} display_m5tab5_config_t;

typedef struct {
    u8g2_t u8g2;
    uint8_t *buffer;
    uint8_t *shadow;
    uint16_t *framebuffer;
    esp_lcd_panel_handle_t panel;
    size_t buffer_size;
    size_t shadow_size;
    uint16_t foreground_rgb565;
    uint16_t background_rgb565;
    esp_err_t last_error;
    uint8_t backlight_percent;
    bool backlight_power;
    bool shadow_valid;
    display_m5tab5_config_t config;
    u8x8_display_info_t display_info;
    uint16_t tile_width;
    uint16_t tile_height;
    size_t buffer_row_bytes;
} display_m5tab5_t;

esp_err_t display_m5tab5_init(display_m5tab5_t *display, const display_m5tab5_config_t *config);
esp_err_t display_m5tab5_resume(display_m5tab5_t *display);
void display_m5tab5_deinit(display_m5tab5_t *display);
void display_m5tab5_invalidate_shadow(display_m5tab5_t *display);
u8g2_t *display_m5tab5_get_u8g2(display_m5tab5_t *display);
bool display_m5tab5_backlight_supported(void);
esp_err_t display_m5tab5_get_backlight(const display_m5tab5_t *display, uint8_t *percent);
esp_err_t display_m5tab5_set_backlight(display_m5tab5_t *display, uint8_t percent);
esp_err_t display_m5tab5_set_colors(display_m5tab5_t *display,
                                     uint32_t foreground_rgb888,
                                     uint32_t background_rgb888);
esp_err_t display_m5tab5_present_surface(display_m5tab5_t *display,
                                        const solar_os_display_surface_t *surface);
esp_err_t display_m5tab5_present_frame(display_m5tab5_t *display,
                                      const solar_os_display_raster_t *frame);

extern const solar_os_expansion_driver_t solar_os_m5tab5_expansion_driver;

#ifdef __cplusplus
}
#endif
