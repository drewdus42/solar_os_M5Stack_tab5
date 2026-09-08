#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

esp_err_t solar_os_m5tab5_touch_attach(const char *name,
                                      const solar_os_expansion_binding_t *bindings,
                                      size_t binding_count);
esp_err_t solar_os_m5tab5_touch_detach(const char *name);
void touch_m5tab5_poll(void);

extern const solar_os_expansion_driver_t solar_os_m5tab5_touch_expansion_driver;
