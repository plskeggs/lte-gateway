#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
#

cmake_minimum_required(VERSION 3.13.1)
project(lte-ble-gateway)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
zephyr_compile_definitions(PROJECT_NAME=${PROJECT_NAME})
zephyr_compile_definitions(_POSIX_C_SOURCE=200809L)

set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(BOARD apricity_gateway_nrf9160)

# This sample runs as a non-secure application on nRF91. Therefore, it
# requires the secure_partition_manager that prepares the required
# peripherals to be available for the application.
#
# Configure the SPM image to enable the peripherals that this sample
# needs.
set(spm_CONF_FILE
  prj.conf
  ${CMAKE_CURRENT_LIST_DIR}/child_secure_partition_manager.conf
  )
  
#include(../ncs/nrf/cmake/boilerplate.cmake)
#include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)

# NORDIC SDK APP START
target_sources(app PRIVATE src/main.c)
# NORDIC SDK APP END
zephyr_include_directories(
src
src/ui
src/watchdog
)

target_sources(app PRIVATE src/ble.c)
target_sources(app PRIVATE src/ble_codec.c)
target_sources(app PRIVATE src/ble_conn_mgr.c)
target_sources(app PRIVATE src/gateway.c)
target_sources(app PRIVATE src/dfu/peripheral_dfu.c)

add_subdirectory(src/ui)
add_subdirectory_ifdef(CONFIG_WATCHDOG src/watchdog)
