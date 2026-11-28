# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(BangTaiPhanLoai C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

set(PEGATRON_IOT_DIR "${CMAKE_CURRENT_LIST_DIR}/dashboard" CACHE PATH "Dashboard project")
set(PEGATRON_FIRMWARE_DIR "${PEGATRON_IOT_DIR}/firmware")
include("${PEGATRON_FIRMWARE_DIR}/configure.cmake")

add_executable(BangTaiPhanLoai main.cpp vl53l1x.cpp
    "${PEGATRON_FIRMWARE_DIR}/pegatron_monitor.c")

pico_set_program_name(BangTaiPhanLoai "BangTaiPhanLoai")
pico_set_program_version(BangTaiPhanLoai "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(BangTaiPhanLoai 0)
pico_enable_stdio_usb(BangTaiPhanLoai 1)

# Add the standard library to the build
target_link_libraries(BangTaiPhanLoai
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(BangTaiPhanLoai PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PEGATRON_FIRMWARE_DIR}
)

# Add any user requested libraries
target_link_libraries(BangTaiPhanLoai 
        hardware_i2c
        hardware_pwm
        hardware_clocks
        hardware_timer
        pico_cyw43_arch_lwip_poll
        pico_lwip_mqtt
        pico_multicore
        pico_flash
        hardware_flash
        pico_rand
        )

pico_add_extra_outputs(BangTaiPhanLoai)

add_executable(TestToF test.cpp vl53l1x.cpp)
target_link_libraries(TestToF PRIVATE pico_stdlib hardware_i2c)
pico_enable_stdio_uart(TestToF 0)
pico_enable_stdio_usb(TestToF 1)
pico_add_extra_outputs(TestToF)

add_executable(TestServoTram1 station1_servo_trial.cpp vl53l1x.cpp)
target_link_libraries(TestServoTram1 PRIVATE pico_stdlib hardware_i2c hardware_pwm hardware_clocks)
pico_enable_stdio_uart(TestServoTram1 0)
pico_enable_stdio_usb(TestServoTram1 1)
pico_add_extra_outputs(TestServoTram1)

add_executable(TestTwoServos test_two_servos.cpp)
target_link_libraries(TestTwoServos PRIVATE pico_stdlib hardware_pwm hardware_clocks)
pico_enable_stdio_uart(TestTwoServos 0)
pico_enable_stdio_usb(TestTwoServos 1)
pico_add_extra_outputs(TestTwoServos)

add_executable(TestRelay test_relay.cpp)
target_link_libraries(TestRelay PRIVATE pico_stdlib)
pico_enable_stdio_uart(TestRelay 0)
pico_enable_stdio_usb(TestRelay 1)
pico_add_extra_outputs(TestRelay)

