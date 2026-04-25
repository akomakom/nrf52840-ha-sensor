cmake_minimum_required(VERSION 3.20.0)

# Tell Zephyr to search for boards in this directory (finds boards/nice_nano)
set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Static partition map — tells PM where bootloader/NVRAM/settings live so
# ZBOSS NVRAM is NOT placed inside the Adafruit bootloader region (0xf4000+).
set(PM_STATIC_YML_FILE ${CMAKE_CURRENT_SOURCE_DIR}/pm_static.yml)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nano_temp VERSION 0.1.0)

# Define ZB_ROUTER_ROLE and ZB_BDB_MODE preprocessor macros when CONFIG_ZIGBEE_ROLE_ROUTER=y
# Both are required for ZBOSS to compile router-specific code paths
# ZB_BDB_MODE enables BDB (Base Device Behavior) commissioning mode
if(CONFIG_ZIGBEE_ROLE_ROUTER)
  target_compile_definitions(app PRIVATE ZB_ROUTER_ROLE ZB_BDB_MODE)
endif()

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
