cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")

if(NOT DEFINED SDKCONFIG_DEFAULTS)
    if(DEFINED ENV{SOLAR_OS_BOARD} AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.$ENV{SOLAR_OS_BOARD}")
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.$ENV{SOLAR_OS_BOARD}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(
    COMPILE_DEFINITIONS
    "ESP_IDF_LWIP_HOOK_FILENAME=\"${CMAKE_CURRENT_LIST_DIR}/src/services/solar_os_lwip_hooks.h\""
    APPEND
)
idf_build_set_property(
    COMPILE_DEFINITIONS
    "SOC_I2S_NUM=3"
    APPEND
)
project(solar_os)
