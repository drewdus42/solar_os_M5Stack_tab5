#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

#define SOLAR_OS_M5TAB5_KEYBOARD_ADDRESS 0x6D

esp_err_t solar_os_m5tab5_keyboard_attach(const char *name,
                                          const solar_os_expansion_binding_t *bindings,
                                          size_t binding_count);
esp_err_t solar_os_m5tab5_keyboard_detach(const char *name);

bool solar_os_m5tab5_keyboard_is_attached(void);

esp_err_t solar_os_m5tab5_keyboard_set_backlight(uint8_t percent);
esp_err_t solar_os_m5tab5_keyboard_get_backlight(uint8_t *percent);

esp_err_t solar_os_m5tab5_keyboard_set_rgb_mode(uint8_t mode);
esp_err_t solar_os_m5tab5_keyboard_get_rgb_mode(uint8_t *mode);

esp_err_t solar_os_m5tab5_keyboard_set_rgb(uint8_t r, uint8_t g, uint8_t b);
esp_err_t solar_os_m5tab5_keyboard_get_rgb(uint8_t *r, uint8_t *g, uint8_t *b);
