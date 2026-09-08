#pragma once

#include "solar_os.h"

extern const solar_os_app_t solar_os_keylight_app;

void solar_os_shell_cmd_keylight(solar_os_context_t *ctx, int argc, char **argv);
esp_err_t solar_os_shell_launch_keylight_tui(solar_os_context_t *ctx);
