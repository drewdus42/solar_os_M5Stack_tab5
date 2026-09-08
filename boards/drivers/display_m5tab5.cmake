set(SOLAR_OS_BOARD_DISPLAY_DRIVER "m5tab5")
include("${CMAKE_CURRENT_LIST_DIR}/pwm_esp_idf.cmake")
list(APPEND SOLAR_OS_BOARD_REQUIRED_PACKAGES driver_display_m5tab5)
