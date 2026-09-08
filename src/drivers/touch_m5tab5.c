#include "touch_m5tab5.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "solar_os_display.h"
#include "solar_os_input.h"

typedef struct {
    bool active;
    bool pressed;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int irq_pin;
    uint16_t target_width;
    uint16_t target_height;
    uint8_t pointer_id;
    int16_t x;
    int16_t y;
    solar_os_input_source_t input_source;
    esp_lcd_touch_handle_t touch_handle;
} solar_os_m5tab5_touch_device_t;

static const char *TAG = "touch_m5tab5";
static solar_os_m5tab5_touch_device_t touch;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                solar_os_m5tab5_touch_device_t *device)
{
    bool have_i2c = false;
    bool have_address = false;

    if (bindings == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    device->irq_pin = -1;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(device->i2c_bus, binding->target, sizeof(device->i2c_bus));
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address) {
                return ESP_ERR_INVALID_ARG;
            }
            device->address = (uint8_t)binding->value;
            have_address = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (device->irq_pin < 0) {
                device->irq_pin = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_address ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void clear_device(void)
{
    if (touch.input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(touch.input_source);
    }
    if (touch.touch_handle != NULL) {
        esp_lcd_touch_del(touch.touch_handle);
    }
    memset(&touch, 0, sizeof(touch));
}

esp_err_t solar_os_m5tab5_touch_attach(const char *name,
                                      const solar_os_expansion_binding_t *bindings,
                                      size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (touch.active) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_m5tab5_touch_device_t candidate = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &candidate),
                        TAG, "invalid bindings");

    solar_os_display_target_t target;
    if (!solar_os_display_find_target(SOLAR_OS_DISPLAY_PRIMARY_TARGET, &target) ||
        target.width == 0 || target.height == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    candidate.target_width = target.width;
    candidate.target_height = target.height;

    /* Ensure IO expander is initialized so touch reset line is pulled high */
    if (bsp_i2c_init() == ESP_OK) {
        bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    }

    ESP_RETURN_ON_ERROR(bsp_touch_new(NULL, &candidate.touch_handle),
                        TAG, "touch controller init failed");

    strlcpy(candidate.name, name, sizeof(candidate.name));
    esp_err_t err = solar_os_input_touch_source_open(candidate.name,
                                                     &candidate.input_source);
    if (err != ESP_OK) {
        esp_lcd_touch_del(candidate.touch_handle);
        return err;
    }

    candidate.active = true;
    touch = candidate;
    ESP_LOGI(TAG, "M5Stack Tab5 touch initialized (%s)", name);
    return ESP_OK;
}

esp_err_t solar_os_m5tab5_touch_detach(const char *name)
{
    if (!touch.active || name == NULL || strcmp(touch.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    clear_device();
    return ESP_OK;
}

void touch_m5tab5_poll(void)
{
    if (!touch.active || touch.touch_handle == NULL) {
        return;
    }

    esp_lcd_touch_read_data(touch.touch_handle);

    uint16_t touch_x[1] = {0};
    uint16_t touch_y[1] = {0};
    uint16_t touch_strength[1] = {0};
    uint8_t touch_cnt = 0;

    bool pressed = esp_lcd_touch_get_coordinates(touch.touch_handle,
                                                 touch_x,
                                                 touch_y,
                                                 touch_strength,
                                                 &touch_cnt,
                                                 1) && (touch_cnt > 0);

    uint16_t sample_x = 0;
    uint16_t sample_y = 0;
    if (pressed) {
        /* Raw coordinates from sensor: 720 wide x 1280 high (portrait).
         * Map to landscape 1280 wide x 720 high:
         * logical_x = 1279 - raw_y
         * logical_y = raw_x
         */
        if (touch_y[0] < 1280U) {
            sample_x = (uint16_t)(1279U - touch_y[0]);
        } else {
            sample_x = 0;
        }
        if (touch_x[0] < 720U) {
            sample_y = touch_x[0];
        } else {
            sample_y = 719U;
        }
    }

    solar_os_input_pointer_action_t action;
    if (pressed && !touch.pressed) {
        action = SOLAR_OS_INPUT_POINTER_PRESS;
    } else if (!pressed && touch.pressed) {
        action = SOLAR_OS_INPUT_POINTER_RELEASE;
    } else if (pressed &&
               (sample_x != (uint16_t)touch.x ||
                sample_y != (uint16_t)touch.y)) {
        action = SOLAR_OS_INPUT_POINTER_MOVE;
    } else {
        return;
    }

    const int16_t next_x = pressed ? (int16_t)sample_x : touch.x;
    const int16_t next_y = pressed ? (int16_t)sample_y : touch.y;
    solar_os_input_pointer_event_t event = {
        .pointer_id = touch.pointer_id,
        .buttons = pressed ? SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY : 0,
        .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = action,
        .x = next_x,
        .y = next_y,
        .delta_x = (int16_t)(next_x - touch.x),
        .delta_y = (int16_t)(next_y - touch.y),
    };
    strlcpy(event.target, SOLAR_OS_DISPLAY_PRIMARY_TARGET, sizeof(event.target));
    if (solar_os_input_write_pointer(touch.input_source, &event) == ESP_OK) {
        touch.pressed = pressed;
        touch.x = next_x;
        touch.y = next_y;
    }
}

static const int addresses[] = {0x5D, 0x14, 0x55};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x5d|0x14|0x55", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq", .required = false},
};

const solar_os_expansion_driver_t solar_os_m5tab5_touch_expansion_driver = {
    .name = "m5tab5-touch",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "M5Stack Tab5 touch controller",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_m5tab5_touch_attach,
    .detach = solar_os_m5tab5_touch_detach,
};
