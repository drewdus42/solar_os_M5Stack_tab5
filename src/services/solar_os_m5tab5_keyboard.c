#include "solar_os_m5tab5_keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "solar_os_buses.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_task.h"

#define M5_TAB5_KB_REG_INT_CFG          0x00U
#define M5_TAB5_KB_REG_INT_STA          0x01U
#define M5_TAB5_KB_REG_EVENT_NUM        0x02U
#define M5_TAB5_KB_REG_BRIGHTNESS       0x03U
#define M5_TAB5_KB_REG_KEYBOARD_MODE    0x10U
#define M5_TAB5_KB_REG_RGB_MODE         0x11U
#define M5_TAB5_KB_REG_CHAR_EVENT_LEN   0x40U
#define M5_TAB5_KB_REG_CHAR_EVENT_BASE  0x50U
#define M5_TAB5_KB_REG_RGB_COLOR_BASE   0x60U

#define M5_TAB5_KB_MODE_STRING          0x02U

#define M5TAB5_KEYBOARD_POLL_MS         15U
#define M5TAB5_KEYBOARD_TASK_STACK      3072U
#define M5TAB5_KEYBOARD_TASK_PRIORITY   (tskIDLE_PRIORITY + 1)

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int irq_pin;
    bool isr_installed;
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    uint32_t keys;
    uint32_t dropped;
    uint32_t bus_errors;
    uint8_t brightness;
    uint8_t rgb_mode;
    uint8_t rgb_r;
    uint8_t rgb_g;
    uint8_t rgb_b;
    bool nvs_loaded;
} solar_os_m5tab5_keyboard_device_t;

static const char *TAG = "m5tab5_kb";
static solar_os_m5tab5_keyboard_device_t kb_device;

static bool binding_role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address,
                                int *irq_pin)
{
    bool have_i2c = false;
    bool have_address = false;

    if (bindings == NULL || i2c_bus == NULL || address == NULL || irq_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *address = 0U;
    *irq_pin = -1;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address || binding->value != SOLAR_OS_M5TAB5_KEYBOARD_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            *address = (uint8_t)binding->value;
            have_address = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (binding_role_is(binding, "irq")) {
                if (*irq_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                *irq_pin = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_address &&
            solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t read_reg(solar_os_m5tab5_keyboard_device_t *device, uint8_t reg, uint8_t *val)
{
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, reg, val, 1);
}

static esp_err_t write_reg(solar_os_m5tab5_keyboard_device_t *device, uint8_t reg, uint8_t val)
{
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, reg, &val, 1);
}

static esp_err_t read_bytes(solar_os_m5tab5_keyboard_device_t *device, uint8_t reg, uint8_t *buf, size_t len)
{
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, reg, buf, len);
}

static const char *KB_NVS_NAMESPACE = "m5tab5_kb";
static const char *KB_NVS_BRIGHTNESS = "brightness";
static const char *KB_NVS_RGB_MODE = "rgb_mode";
static const char *KB_NVS_RGB_COLOR = "rgb_color";

#define DEFAULT_KEYBOARD_BRIGHTNESS 20U
#define DEFAULT_KEYBOARD_RGB_MODE   1U
#define DEFAULT_KEYBOARD_RGB_COLOR  0x000000U

static void load_nvs_settings(solar_os_m5tab5_keyboard_device_t *device)
{
    device->brightness = DEFAULT_KEYBOARD_BRIGHTNESS;
    device->rgb_mode = DEFAULT_KEYBOARD_RGB_MODE;
    device->rgb_r = 0;
    device->rgb_g = 0;
    device->rgb_b = 0;
    device->nvs_loaded = true;

    nvs_handle_t nvs;
    if (nvs_open(KB_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t b = 0;
        if (nvs_get_u8(nvs, KB_NVS_BRIGHTNESS, &b) == ESP_OK && b <= 100) {
            device->brightness = b;
        }
        uint8_t m = 0;
        if (nvs_get_u8(nvs, KB_NVS_RGB_MODE, &m) == ESP_OK && m <= 1) {
            device->rgb_mode = m;
        }
        uint32_t c = 0;
        if (nvs_get_u32(nvs, KB_NVS_RGB_COLOR, &c) == ESP_OK) {
            device->rgb_r = (uint8_t)((c >> 16) & 0xFF);
            device->rgb_g = (uint8_t)((c >> 8) & 0xFF);
            device->rgb_b = (uint8_t)(c & 0xFF);
        }
        nvs_close(nvs);
    }
}

static void save_nvs_settings(const solar_os_m5tab5_keyboard_device_t *device)
{
    nvs_handle_t nvs;
    if (nvs_open(KB_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, KB_NVS_BRIGHTNESS, device->brightness);
        (void)nvs_set_u8(nvs, KB_NVS_RGB_MODE, device->rgb_mode);
        uint32_t c = ((uint32_t)device->rgb_r << 16) |
                     ((uint32_t)device->rgb_g << 8) |
                     (uint32_t)device->rgb_b;
        (void)nvs_set_u32(nvs, KB_NVS_RGB_COLOR, c);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static esp_err_t configure_keyboard(solar_os_m5tab5_keyboard_device_t *device)
{
    uint8_t dummy = 0;
    (void)read_reg(device, M5_TAB5_KB_REG_INT_CFG, &dummy);

    /* Switch keyboard MCU to String Mode */
    ESP_RETURN_ON_ERROR(write_reg(device, M5_TAB5_KB_REG_KEYBOARD_MODE, M5_TAB5_KB_MODE_STRING),
                        TAG, "set mode failed");

    if (!device->nvs_loaded) {
        load_nvs_settings(device);
    }

    /* Apply configured/persisted backlight brightness */
    (void)write_reg(device, M5_TAB5_KB_REG_BRIGHTNESS, device->brightness);

    /* Apply configured/persisted RGB mode */
    (void)write_reg(device, M5_TAB5_KB_REG_RGB_MODE, device->rgb_mode);

    /* If custom RGB mode, apply color (format: B, G, R, 0, B, G, R) */
    if (device->rgb_mode == 1U) {
        uint8_t rgb_buf[7] = {
            device->rgb_b, device->rgb_g, device->rgb_r,
            0x00,
            device->rgb_b, device->rgb_g, device->rgb_r
        };
        (void)solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                         M5_TAB5_KB_REG_RGB_COLOR_BASE, rgb_buf, 7);
    }

    /* Clear event queue and interrupt status */
    (void)write_reg(device, M5_TAB5_KB_REG_EVENT_NUM, 0U);
    (void)write_reg(device, M5_TAB5_KB_REG_INT_STA, 0U);

    return ESP_OK;
}

static void dispatch_char(solar_os_m5tab5_keyboard_device_t *device, uint8_t ch, uint8_t modifier)
{
    if (ch == 0U) {
        return;
    }

    uint8_t key = ch;

    switch (ch) {
    case '\r':
    case '\n':
        key = SOLAR_OS_KEY_ENTER;
        break;
    case '\b':
        key = '\b';
        break;
    case '\t':
        key = '\t';
        break;
    case 0x1BU:
        key = SOLAR_OS_KEY_ESCAPE;
        break;
    case 0x7FU:
    case SOLAR_OS_KEY_DELETE:
        key = SOLAR_OS_KEY_DELETE;
        break;
    case 0x11U: /* UP */
    case SOLAR_OS_KEY_UP:
        key = (modifier & 0x01U) ? SOLAR_OS_KEY_CTRL_UP : SOLAR_OS_KEY_UP;
        break;
    case 0x12U: /* DOWN */
    case SOLAR_OS_KEY_DOWN:
        key = (modifier & 0x01U) ? SOLAR_OS_KEY_CTRL_DOWN : SOLAR_OS_KEY_DOWN;
        break;
    case 0x13U: /* RIGHT */
    case SOLAR_OS_KEY_RIGHT:
        key = (modifier & 0x01U) ? SOLAR_OS_KEY_CTRL_RIGHT : SOLAR_OS_KEY_RIGHT;
        break;
    case 0x14U: /* LEFT */
    case SOLAR_OS_KEY_LEFT:
        key = (modifier & 0x01U) ? SOLAR_OS_KEY_CTRL_LEFT : SOLAR_OS_KEY_LEFT;
        break;
    case SOLAR_OS_KEY_HOME:
        key = (modifier & 0x01U) ? SOLAR_OS_KEY_CTRL_HOME : SOLAR_OS_KEY_HOME;
        break;
    case SOLAR_OS_KEY_END:
        key = (modifier & 0x01U) ? SOLAR_OS_KEY_CTRL_END : SOLAR_OS_KEY_END;
        break;
    case SOLAR_OS_KEY_PAGE_UP:
    case SOLAR_OS_KEY_PAGE_DOWN:
        key = ch;
        break;
    default:
        if ((modifier & 0x01U) != 0) { /* Ctrl modifier */
            if (ch >= 'a' && ch <= 'z') {
                key = (uint8_t)(ch - 'a' + 1);
            } else if (ch >= 'A' && ch <= 'Z') {
                key = (uint8_t)(ch - 'A' + 1);
            }
        }
        break;
    }

    /* If ALT modifier is active (bit 2), emit ALT prefix before key */
    if ((modifier & 0x04U) != 0) {
        (void)solar_os_input_write_char(device->input_source, (char)SOLAR_OS_KEY_ALT_PREFIX);
    }

    ESP_LOGD(TAG, "Key: 0x%02X ('%c'), mod: 0x%02X", key, (key >= 32 && key <= 126) ? key : '.', modifier);

    if (solar_os_input_write_char(device->input_source, (char)key) != ESP_OK) {
        device->dropped++;
    } else {
        device->keys++;
    }
}

static void dispatch_key_token(solar_os_m5tab5_keyboard_device_t *device, const char *str, size_t len, uint8_t modifier)
{
    if (device == NULL || str == NULL || len == 0) {
        return;
    }

    /* Single-character token */
    if (len == 1) {
        dispatch_char(device, (uint8_t)str[0], modifier);
        return;
    }

    /* Multi-character tokens: check functional keys */
    if (strcasecmp(str, "enter") == 0 || strcasecmp(str, "ent") == 0 ||
        strcmp(str, "\r\n") == 0 || strcmp(str, "\n") == 0 || strcmp(str, "\r") == 0) {
        dispatch_char(device, (uint8_t)SOLAR_OS_KEY_ENTER, modifier);
        return;
    }

    if (strcasecmp(str, "backspace") == 0 || strcasecmp(str, "bs") == 0 ||
        strcasecmp(str, "bksp") == 0 || strcmp(str, "\b") == 0) {
        dispatch_char(device, (uint8_t)'\b', modifier);
        return;
    }

    if (strcasecmp(str, "del") == 0 || strcasecmp(str, "delete") == 0) {
        dispatch_char(device, (uint8_t)SOLAR_OS_KEY_DELETE, modifier);
        return;
    }

    if (strcasecmp(str, "esc") == 0 || strcasecmp(str, "escape") == 0) {
        dispatch_char(device, (uint8_t)SOLAR_OS_KEY_ESCAPE, modifier);
        return;
    }

    if (strcasecmp(str, "tab") == 0) {
        dispatch_char(device, (uint8_t)'\t', modifier);
        return;
    }

    if (strcasecmp(str, "space") == 0 || strcasecmp(str, "spc") == 0) {
        dispatch_char(device, (uint8_t)' ', modifier);
        return;
    }

    if (strcasecmp(str, "up") == 0) {
        dispatch_char(device, 0x11U, modifier);
        return;
    }

    if (strcasecmp(str, "down") == 0 || strcasecmp(str, "dn") == 0) {
        dispatch_char(device, 0x12U, modifier);
        return;
    }

    if (strcasecmp(str, "left") == 0 || strcasecmp(str, "lt") == 0) {
        dispatch_char(device, 0x14U, modifier);
        return;
    }

    if (strcasecmp(str, "right") == 0 || strcasecmp(str, "rt") == 0) {
        dispatch_char(device, 0x13U, modifier);
        return;
    }

    if (strcasecmp(str, "home") == 0) {
        dispatch_char(device, (uint8_t)SOLAR_OS_KEY_HOME, modifier);
        return;
    }

    if (strcasecmp(str, "end") == 0) {
        dispatch_char(device, (uint8_t)SOLAR_OS_KEY_END, modifier);
        return;
    }

    if (strcasecmp(str, "pgup") == 0 || strcasecmp(str, "pageup") == 0) {
        dispatch_char(device, (uint8_t)SOLAR_OS_KEY_PAGE_UP, modifier);
        return;
    }

    if (strcasecmp(str, "pgdn") == 0 || strcasecmp(str, "pagedown") == 0) {
        dispatch_char(device, (uint8_t)SOLAR_OS_KEY_PAGE_DOWN, modifier);
        return;
    }

    /* Standalone modifier / state keys: ignore */
    if (strcasecmp(str, "ctrl") == 0 || strcasecmp(str, "alt") == 0 ||
        strcasecmp(str, "shift") == 0 || strcasecmp(str, "sym") == 0 ||
        strcasecmp(str, "fn") == 0 || strcasecmp(str, "aa") == 0 ||
        strcasecmp(str, "caps") == 0 || strcasecmp(str, "capslock") == 0) {
        return;
    }

    /* Multi-character text payload: dispatch individual characters */
    for (size_t i = 0; i < len; i++) {
        if (str[i] != '\0') {
            dispatch_char(device, (uint8_t)str[i], modifier);
        }
    }
}

static esp_err_t poll_once(solar_os_m5tab5_keyboard_device_t *device)
{
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_reg(device, M5_TAB5_KB_REG_INT_STA, &status), TAG, "INT_STA read failed");

    if ((status & 0x07U) == 0U) {
        return ESP_OK;
    }

    uint8_t count = 0;
    ESP_RETURN_ON_ERROR(read_reg(device, M5_TAB5_KB_REG_EVENT_NUM, &count), TAG, "EVENT_NUM read failed");

    uint32_t drained = 0;
    while (count > 0 && drained < 32) {
        uint8_t len = 0;
        if (read_reg(device, M5_TAB5_KB_REG_CHAR_EVENT_LEN, &len) == ESP_OK && len > 0) {
            if (len > 15) {
                len = 15;
            }
            uint8_t buf[16];
            if (read_bytes(device, M5_TAB5_KB_REG_CHAR_EVENT_BASE, buf, len + 1) == ESP_OK) {
                const uint8_t modifier = buf[0];
                char str[16];
                memcpy(str, &buf[1], len);
                str[len] = '\0';
                dispatch_key_token(device, str, len, modifier);
            }
        }
        count--;
        drained++;
    }

    return write_reg(device, M5_TAB5_KB_REG_INT_STA, 0U);
}

static void m5tab5_keyboard_worker(void *arg)
{
    solar_os_m5tab5_keyboard_device_t *device = arg;
    bool bus_error_reported = false;

    while (!device->stop_requested) {
        const esp_err_t err = poll_once(device);
        if (err == ESP_OK) {
            if (bus_error_reported) {
                ESP_LOGI(TAG, "%s bus recovered, reconfiguring", device->name);
                (void)configure_keyboard(device);
                bus_error_reported = false;
            }
        } else {
            device->bus_errors++;
            if (!bus_error_reported) {
                ESP_LOGW(TAG, "%s poll failed on %s: %s", device->name, device->i2c_bus, esp_err_to_name(err));
                bus_error_reported = true;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(M5TAB5_KEYBOARD_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void IRAM_ATTR m5tab5_keyboard_isr(void *arg)
{
    solar_os_m5tab5_keyboard_device_t *device = arg;
    if (device != NULL && device->worker_task != NULL) {
        BaseType_t higher_woken = pdFALSE;
        vTaskNotifyGiveFromISR(device->worker_task, &higher_woken);
        if (higher_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

static void clear_device(solar_os_m5tab5_keyboard_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->isr_installed && device->irq_pin >= 0) {
        (void)gpio_isr_handler_remove((gpio_num_t)device->irq_pin);
    }
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
    device->irq_pin = -1;
}

esp_err_t solar_os_m5tab5_keyboard_attach(const char *name,
                                          const solar_os_expansion_binding_t *bindings,
                                          size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    int irq_pin = -1;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (kb_device.active) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, i2c_bus, sizeof(i2c_bus), &address, &irq_pin),
                        TAG, "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address), TAG, "M5Tab5 keyboard not found");

    clear_device(&kb_device);
    kb_device.address = address;
    kb_device.irq_pin = irq_pin;
    strlcpy(kb_device.name, name, sizeof(kb_device.name));
    strlcpy(kb_device.i2c_bus, i2c_bus, sizeof(kb_device.i2c_bus));

    esp_err_t err = configure_keyboard(&kb_device);
    if (err != ESP_OK) {
        clear_device(&kb_device);
        return err;
    }

    err = solar_os_input_keyboard_source_open(kb_device.name, true, &kb_device.input_source);
    if (err != ESP_OK) {
        clear_device(&kb_device);
        return err;
    }

    if (solar_os_task_create_pinned_internal(m5tab5_keyboard_worker,
                                             kb_device.name,
                                             M5TAB5_KEYBOARD_TASK_STACK,
                                             &kb_device,
                                             M5TAB5_KEYBOARD_TASK_PRIORITY,
                                             &kb_device.worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(&kb_device);
        return ESP_ERR_NO_MEM;
    }

    if (irq_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (uint32_t)irq_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        err = gpio_config(&config);
        if (err == ESP_OK) {
            (void)gpio_install_isr_service(0);
            if (gpio_isr_handler_add((gpio_num_t)irq_pin, m5tab5_keyboard_isr, &kb_device) == ESP_OK) {
                kb_device.isr_installed = true;
            }
        }
    }

    kb_device.active = true;
    ESP_LOGI(TAG, "%s attached on %s address 0x%02x (irq=%d)", name, i2c_bus, address, irq_pin);
    return ESP_OK;
}

esp_err_t solar_os_m5tab5_keyboard_detach(const char *name)
{
    if (!kb_device.active || name == NULL || strcmp(kb_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    kb_device.stop_requested = true;
    if (kb_device.worker_task != NULL) {
        (void)xTaskNotifyGive(kb_device.worker_task);
    }
    if (!solar_os_task_wait_done(kb_device.worker_task, &kb_device.worker_done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "%s detached: %lu keys, %lu dropped, %lu bus errors",
             name,
             (unsigned long)kb_device.keys,
             (unsigned long)kb_device.dropped,
             (unsigned long)kb_device.bus_errors);
    clear_device(&kb_device);
    return ESP_OK;
}

bool solar_os_m5tab5_keyboard_is_attached(void)
{
    return kb_device.active;
}

esp_err_t solar_os_m5tab5_keyboard_set_backlight(uint8_t percent)
{
    if (!kb_device.active) {
        return ESP_ERR_NOT_FOUND;
    }
    if (percent > 100) {
        percent = 100;
    }
    esp_err_t err = write_reg(&kb_device, M5_TAB5_KB_REG_BRIGHTNESS, percent);
    if (err == ESP_OK) {
        kb_device.brightness = percent;
        save_nvs_settings(&kb_device);
    }
    return err;
}

esp_err_t solar_os_m5tab5_keyboard_get_backlight(uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!kb_device.active) {
        return ESP_ERR_NOT_FOUND;
    }
    *percent = kb_device.brightness;
    return ESP_OK;
}

esp_err_t solar_os_m5tab5_keyboard_set_rgb_mode(uint8_t mode)
{
    if (!kb_device.active) {
        return ESP_ERR_NOT_FOUND;
    }
    if (mode > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = write_reg(&kb_device, M5_TAB5_KB_REG_RGB_MODE, mode);
    if (err == ESP_OK) {
        kb_device.rgb_mode = mode;
        save_nvs_settings(&kb_device);
    }
    return err;
}

esp_err_t solar_os_m5tab5_keyboard_get_rgb_mode(uint8_t *mode)
{
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!kb_device.active) {
        return ESP_ERR_NOT_FOUND;
    }
    *mode = kb_device.rgb_mode;
    return ESP_OK;
}

esp_err_t solar_os_m5tab5_keyboard_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!kb_device.active) {
        return ESP_ERR_NOT_FOUND;
    }
    if (kb_device.rgb_mode != 1U) {
        (void)write_reg(&kb_device, M5_TAB5_KB_REG_RGB_MODE, 1U);
        kb_device.rgb_mode = 1U;
    }
    uint8_t rgb_buf[7] = {b, g, r, 0x00, b, g, r};
    esp_err_t err = solar_os_bus_i2c_write_reg(kb_device.i2c_bus, kb_device.address,
                                              M5_TAB5_KB_REG_RGB_COLOR_BASE, rgb_buf, 7);
    if (err == ESP_OK) {
        kb_device.rgb_r = r;
        kb_device.rgb_g = g;
        kb_device.rgb_b = b;
        save_nvs_settings(&kb_device);
    }
    return err;
}

esp_err_t solar_os_m5tab5_keyboard_get_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r == NULL || g == NULL || b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!kb_device.active) {
        return ESP_ERR_NOT_FOUND;
    }
    *r = kb_device.rgb_r;
    *g = kb_device.rgb_g;
    *b = kb_device.rgb_b;
    return ESP_OK;
}
