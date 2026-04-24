cmake_minimum_required(VERSION 3.20.0)

# Tell Zephyr to search for boards in this directory (finds boards/nice_nano)
set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Static partition map — tells PM where bootloader/NVRAM/settings live so
# ZBOSS NVRAM is NOT placed inside the Adafruit bootloader region (0xf4000+).
set(PM_STATIC_YML_FILE ${CMAKE_CURRENT_SOURCE_DIR}/pm_static.yml)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nano_temp VERSION 0.1.0)

target_sources(app PRIVATE src/main.c)
