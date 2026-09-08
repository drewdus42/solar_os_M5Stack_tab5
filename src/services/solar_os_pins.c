#include "solar_os_pins.h"

#include <stdint.h>
#include <string.h>

#include "solar_os_board.h"
#include "solar_os_board_caps.h"

#if SOLAR_OS_BOARD_HAS_GPIO
static const solar_os_board_pin_t board_pins[] = SOLAR_OS_BOARD_GPIO_SLOTS;
#endif

#if SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT > 0
static const solar_os_board_connector_pin_t connector_pins[] =
    SOLAR_OS_BOARD_CONNECTOR_PINS;

_Static_assert(sizeof(connector_pins) / sizeof(connector_pins[0]) ==
                   SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT,
               "connector pin count does not match board layout");
#endif

static bool mask_contains(uint64_t mask, int pin)
{
    return pin >= 0 && pin < 64 && (mask & (1ULL << (uint32_t)pin)) != 0;
}

size_t solar_os_pin_count(void)
{
#if SOLAR_OS_BOARD_HAS_GPIO
    return sizeof(board_pins) / sizeof(board_pins[0]);
#else
    return 0;
#endif
}

bool solar_os_pin_get_info(size_t index, solar_os_pin_info_t *info)
{
#if SOLAR_OS_BOARD_HAS_GPIO
    if (info == NULL || index >= solar_os_pin_count()) {
        return false;
    }

    const solar_os_board_pin_t *pin = &board_pins[index];
    *info = (solar_os_pin_info_t) {
        .pin = pin->pin,
        .expansion = mask_contains(SOLAR_OS_BOARD_EXPANSION_GPIO_MASK, pin->pin),
        .direct_gpio = (pin->policy == SOLAR_OS_PIN_POLICY_FREE &&
                        mask_contains(SOLAR_OS_BOARD_USER_GPIO_MASK, pin->pin)) ||
            (pin->policy == SOLAR_OS_PIN_POLICY_RELEASABLE &&
             mask_contains(SOLAR_OS_BOARD_EXPANSION_GPIO_MASK, pin->pin)),
        .policy = pin->policy,
        .role = pin->role,
    };
    return true;
#else
    (void)index;
    (void)info;
    return false;
#endif
}

bool solar_os_pin_get_info_by_pin(int pin, solar_os_pin_info_t *info)
{
    for (size_t i = 0; i < solar_os_pin_count(); i++) {
        solar_os_pin_info_t candidate;
        if (solar_os_pin_get_info(i, &candidate) && candidate.pin == pin) {
            if (info != NULL) {
                *info = candidate;
            }
            return true;
        }
    }
    return false;
}

bool solar_os_pin_is_expansion(int pin)
{
    solar_os_pin_info_t info;
    return solar_os_pin_get_info_by_pin(pin, &info) && info.expansion;
}

bool solar_os_pin_is_direct_gpio(int pin)
{
    solar_os_pin_info_t info;
    return solar_os_pin_get_info_by_pin(pin, &info) && info.direct_gpio;
}

bool solar_os_pin_is_routable(int pin)
{
    solar_os_pin_info_t info;
    return solar_os_pin_get_info_by_pin(pin, &info) &&
        info.expansion &&
        info.policy != SOLAR_OS_PIN_POLICY_FIXED;
}

const char *solar_os_pin_policy_name(solar_os_pin_policy_t policy)
{
    switch (policy) {
    case SOLAR_OS_PIN_POLICY_FIXED:
        return "fixed";
    case SOLAR_OS_PIN_POLICY_RELEASABLE:
        return "releasable";
    case SOLAR_OS_PIN_POLICY_FREE:
        return "free";
    default:
        return "unknown";
    }
}

bool solar_os_connector_layout_get_info(solar_os_connector_layout_info_t *info)
{
    if (info == NULL || SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT == 0 ||
        SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS == 0 ||
        SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS == 0) {
        return false;
    }
    *info = (solar_os_connector_layout_info_t) {
        .title = SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE,
        .view = SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW,
        .rows = SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS,
        .columns = SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS,
    };
    return true;
}

size_t solar_os_connector_pin_count(void)
{
    return SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT;
}

bool solar_os_connector_pin_get_info(size_t index, solar_os_connector_pin_info_t *info)
{
#if SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT > 0
    if (info == NULL || index >= solar_os_connector_pin_count()) {
        return false;
    }
    *info = connector_pins[index];
    return true;
#else
    (void)index;
    (void)info;
    return false;
#endif
}

bool solar_os_connector_pin_find(size_t row,
                                 size_t column,
                                 solar_os_connector_pin_info_t *info,
                                 size_t *index)
{
    for (size_t i = 0; i < solar_os_connector_pin_count(); i++) {
        solar_os_connector_pin_info_t candidate = {0};
        if (solar_os_connector_pin_get_info(i, &candidate) &&
            candidate.row == row && candidate.column == column) {
            if (info != NULL) {
                *info = candidate;
            }
            if (index != NULL) {
                *index = i;
            }
            return true;
        }
    }
    return false;
}

bool solar_os_connector_exists(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < solar_os_connector_pin_count(); i++) {
        solar_os_connector_pin_info_t pin = {0};
        if (solar_os_connector_pin_get_info(i, &pin) &&
            pin.connector != NULL && strcmp(pin.connector, name) == 0) {
            return true;
        }
    }
    return false;
}

const char *solar_os_connector_pin_kind_name(solar_os_connector_pin_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_CONNECTOR_PIN_GPIO:
        return "GPIO";
    case SOLAR_OS_CONNECTOR_PIN_POWER:
        return "power";
    case SOLAR_OS_CONNECTOR_PIN_GROUND:
        return "ground";
    case SOLAR_OS_CONNECTOR_PIN_CONTROL:
        return "control";
    case SOLAR_OS_CONNECTOR_PIN_NC:
        return "not connected";
    default:
        return "unknown";
    }
}
