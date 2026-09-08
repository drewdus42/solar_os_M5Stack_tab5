#include "rtc_rx8130.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "solar_os_buses.h"
#include "solar_os_rtc.h"

#define RX8130_REG_SEC   0x10U
#define RX8130_REG_MIN   0x11U
#define RX8130_REG_HOUR  0x12U
#define RX8130_REG_WDAY  0x13U
#define RX8130_REG_MDAY  0x14U
#define RX8130_REG_MONTH 0x15U
#define RX8130_REG_YEAR  0x16U
#define RX8130_REG_FLAG  0x1DU
#define RX8130_REG_CTRL0 0x1EU
#define RX8130_REG_CTRL1 0x1FU

#define RX8130_BIT_FLAG_VLF  (1U << 1)
#define RX8130_BIT_CTRL_STOP (1U << 6)

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int irq_pin;
} solar_os_rx8130_device_t;

static const char *TAG = "rx8130";
static solar_os_rx8130_device_t rx8130_device;

static inline uint8_t bcd_to_dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10U) + (val & 0x0FU));
}

static inline uint8_t dec_to_bcd(uint8_t val)
{
    return (uint8_t)(((val / 10U) << 4) | (val % 10U));
}

static esp_err_t rtc_get(void *user, solar_os_datetime_t *datetime)
{
    solar_os_rx8130_device_t *device = (solar_os_rx8130_device_t *)user;
    if (device == NULL || !device->active || datetime == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[7] = {0};
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_read_reg(device->i2c_bus, device->address,
                                                  RX8130_REG_SEC, data, sizeof(data)),
                        TAG, "read time failed");

    uint8_t flag = 0;
    bool integrity = true;
    if (solar_os_bus_i2c_read_reg(device->i2c_bus, device->address,
                                  RX8130_REG_FLAG, &flag, 1) == ESP_OK) {
        if ((flag & RX8130_BIT_FLAG_VLF) != 0U) {
            integrity = false;
        }
    }

    datetime->second = bcd_to_dec(data[0] & 0x7FU);
    datetime->minute = bcd_to_dec(data[1] & 0x7FU);
    datetime->hour   = bcd_to_dec(data[2] & 0x3FU);
    datetime->weekday = bcd_to_dec(data[3] & 0x7FU);
    datetime->day    = bcd_to_dec(data[4] & 0x3FU);
    datetime->month  = bcd_to_dec(data[5] & 0x1FU);
    datetime->year   = (uint16_t)(2000U + bcd_to_dec(data[6]));
    datetime->clock_integrity = integrity;

    return ESP_OK;
}

static esp_err_t rtc_set(void *user, const solar_os_datetime_t *datetime)
{
    solar_os_rx8130_device_t *device = (solar_os_rx8130_device_t *)user;
    if (device == NULL || !device->active || datetime == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Set STOP bit before changing clock */
    uint8_t ctrl0 = 0;
    (void)solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, RX8130_REG_CTRL0, &ctrl0, 1);
    ctrl0 |= RX8130_BIT_CTRL_STOP;
    (void)solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, RX8130_REG_CTRL0, &ctrl0, 1);

    uint8_t data[7] = {
        dec_to_bcd(datetime->second % 60U),
        dec_to_bcd(datetime->minute % 60U),
        dec_to_bcd(datetime->hour % 24U),
        dec_to_bcd(datetime->weekday % 7U),
        dec_to_bcd(datetime->day >= 1U ? datetime->day : 1U),
        dec_to_bcd(datetime->month >= 1U ? datetime->month : 1U),
        dec_to_bcd((uint8_t)(datetime->year >= 2000U ? (datetime->year - 2000U) : 0U)),
    };
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                                   RX8130_REG_SEC, data, sizeof(data)),
                        TAG, "write time failed");

    /* Clear VLF flag */
    uint8_t flag = 0;
    if (solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, RX8130_REG_FLAG, &flag, 1) == ESP_OK) {
        flag &= ~RX8130_BIT_FLAG_VLF;
        (void)solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, RX8130_REG_FLAG, &flag, 1);
    }

    /* Clear STOP bit */
    ctrl0 &= ~RX8130_BIT_CTRL_STOP;
    (void)solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, RX8130_REG_CTRL0, &ctrl0, 1);

    return ESP_OK;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                solar_os_rx8130_device_t *device)
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

    return (have_i2c && have_address) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_rx8130_attach(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (rx8130_device.active) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_rx8130_device_t candidate = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &candidate),
                        TAG, "invalid bindings");

    /* Enable battery backup switchover & charging: set bits 4 and 5 in CTRL1 */
    uint8_t ctrl1 = 0;
    if (solar_os_bus_i2c_read_reg(candidate.i2c_bus, candidate.address,
                                  RX8130_REG_CTRL1, &ctrl1, 1) == ESP_OK) {
        ctrl1 |= (1U << 4) | (1U << 5);
        (void)solar_os_bus_i2c_write_reg(candidate.i2c_bus, candidate.address,
                                         RX8130_REG_CTRL1, &ctrl1, 1);
    }

    strlcpy(candidate.name, name, sizeof(candidate.name));
    candidate.active = true;
    rx8130_device = candidate;

    const solar_os_rtc_provider_t provider = {
        .get_utc_datetime = rtc_get,
        .set_utc_datetime = rtc_set,
        .user = &rx8130_device,
        .interrupt_gpio = rx8130_device.irq_pin,
        .interrupt_active_level = 0,
    };
    const esp_err_t ret = solar_os_rtc_register_provider(name, &provider);
    if (ret != ESP_OK) {
        memset(&rx8130_device, 0, sizeof(rx8130_device));
        return ret;
    }

    ESP_LOGI(TAG, "RX8130 RTC attached (%s on %s@0x%02x)",
             name, rx8130_device.i2c_bus, rx8130_device.address);
    return ESP_OK;
}

esp_err_t solar_os_rx8130_detach(const char *name)
{
    if (!rx8130_device.active || name == NULL || strcmp(rx8130_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(solar_os_rtc_unregister_provider(name),
                        TAG, "unregister provider failed");
    memset(&rx8130_device, 0, sizeof(rx8130_device));
    return ESP_OK;
}

static const int addresses[] = {0x32};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x32", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq", .required = false},
};

const solar_os_expansion_driver_t solar_os_rx8130_expansion_driver = {
    .name = "rx8130",
    .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    .summary = "RX8130 real-time clock",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_rx8130_attach,
    .detach = solar_os_rx8130_detach,
};
