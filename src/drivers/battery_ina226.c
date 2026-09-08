#include "battery_ina226.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"

#if defined(SOLAR_OS_BOARD_M5STACK_TAB5)
#include "bsp/m5stack_tab5.h"
#endif

#define INA226_REG_CONFIG       0x00U
#define INA226_REG_SHUNTVOLTAGE 0x01U
#define INA226_REG_BUSVOLTAGE   0x02U
#define INA226_REG_POWER        0x03U
#define INA226_REG_CURRENT      0x04U
#define INA226_REG_CALIBRATION  0x05U

#define INA226_DEFAULT_CONFIG   0x4527U  /* avg 16, 1100us bus/shunt, continuous */
#define INA226_TAB5_CALIBRATION 0x0800U  /* 2048 for 5 mOhm shunt, 8.192 A max */

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
} solar_os_ina226_device_t;

static const char *TAG = "ina226";
static solar_os_ina226_device_t battery_device;

static esp_err_t ina226_read_reg16(const solar_os_ina226_device_t *device,
                                   uint8_t reg,
                                   uint16_t *value)
{
    uint8_t buf[2] = {0};
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, reg, buf, 2),
                        TAG, "read reg 0x%02x failed", reg);
    *value = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    return ESP_OK;
}

static esp_err_t ina226_write_reg16(const solar_os_ina226_device_t *device,
                                    uint8_t reg,
                                    uint16_t value)
{
    uint8_t buf[2] = {(uint8_t)(value >> 8), (uint8_t)(value & 0xffU)};
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, reg, buf, 2);
}

static esp_err_t battery_read(void *user, solar_os_battery_sample_t *sample)
{
    solar_os_ina226_device_t *device = (solar_os_ina226_device_t *)user;
    if (device == NULL || !device->active || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t raw_bus = 0;
    ESP_RETURN_ON_ERROR(ina226_read_reg16(device, INA226_REG_BUSVOLTAGE, &raw_bus),
                        TAG, "read bus voltage failed");

    /* 1 LSB = 1.25 mV: bus_mv = raw * 1.25 = raw * 5 / 4 */
    const uint32_t bus_mv = ((uint32_t)raw_bus * 5U) / 4U;

    int16_t raw_current = 0;
    (void)ina226_read_reg16(device, INA226_REG_CURRENT, (uint16_t *)&raw_current);

    /* Current LSB = 0.25 mA with cal=2048: current_ma = raw_current / 4 */
    const int32_t current_ma = (int32_t)raw_current / 4;
    const bool is_charging = current_ma > 10;
    const bool has_ext_power = is_charging || (bus_mv > 4250U);

    *sample = (solar_os_battery_sample_t){
        .battery_mv = (uint16_t)(bus_mv > UINT16_MAX ? UINT16_MAX : bus_mv),
        .calibrated = true,
        .charging_valid = true,
        .charging = is_charging,
        .external_power_valid = true,
        .external_power = has_ext_power,
    };
    return ESP_OK;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                solar_os_ina226_device_t *device)
{
    bool have_i2c = false;
    bool have_address = false;

    if (bindings == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return (have_i2c && have_address) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_ina226_attach(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (battery_device.active) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_ina226_device_t candidate = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &candidate),
                        TAG, "invalid bindings");

#if defined(SOLAR_OS_BOARD_M5STACK_TAB5)
    /* Turn on PI4IOE rails (charging enable and power switches) */
    if (bsp_i2c_init() == ESP_OK) {
        bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    }
#endif

    ESP_RETURN_ON_ERROR(ina226_write_reg16(&candidate, INA226_REG_CONFIG, INA226_DEFAULT_CONFIG),
                        TAG, "configure INA226 failed");
    ESP_RETURN_ON_ERROR(ina226_write_reg16(&candidate, INA226_REG_CALIBRATION, INA226_TAB5_CALIBRATION),
                        TAG, "calibrate INA226 failed");

    strlcpy(candidate.name, name, sizeof(candidate.name));
    candidate.active = true;
    battery_device = candidate;

    const solar_os_battery_provider_t provider = {
        .read = battery_read,
        .user = &battery_device,
    };
    const esp_err_t ret = solar_os_battery_register_provider(name, &provider);
    if (ret != ESP_OK) {
        memset(&battery_device, 0, sizeof(battery_device));
        return ret;
    }

    ESP_LOGI(TAG, "INA226 battery monitor attached (%s on %s@0x%02x)",
             name, battery_device.i2c_bus, battery_device.address);
    return ESP_OK;
}

esp_err_t solar_os_ina226_detach(const char *name)
{
    if (!battery_device.active || name == NULL || strcmp(battery_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(solar_os_battery_unregister_provider(name),
                        TAG, "unregister provider failed");
    memset(&battery_device, 0, sizeof(battery_device));
    return ESP_OK;
}

static const int addresses[] = {0x40, 0x41, 0x44, 0x45};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x40|0x41|0x44|0x45", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
};

const solar_os_expansion_driver_t solar_os_ina226_expansion_driver = {
    .name = "ina226",
    .category = SOLAR_OS_EXPANSION_CATEGORY_POWER,
    .summary = "INA226 power monitor",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_ina226_attach,
    .detach = solar_os_ina226_detach,
};
