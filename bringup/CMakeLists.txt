cmake_minimum_required(VERSION 3.13)

# Phase 0 bring-up for the Olimex RP2350pc (PLAN.md §3): HSTX video, PWM audio,
# PSRAM, microSD over SPI and a USB keyboard through the hub, each checked on
# its own and reported on the HDMI screen and on UART0 (UEXT1).

set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")
set(PICO_BOARD olimex_rp2350pc CACHE STRING "Board type")
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

include(pico_sdk_import.cmake)

project(ts2068_2350_bringup C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

add_executable(bringup
    main.c
    dvi.c
    console.c
    audio.c
    psram.c
    sd.c
    usbkbd.c
)

target_include_directories(bringup PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(bringup PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(bringup
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_pwm
    hardware_spi
    hardware_vreg
    tinyusb_host
)

# Native USB is the host port (the hub), so stdio goes to UART0 on UEXT1.
pico_enable_stdio_usb(bringup 0)
pico_enable_stdio_uart(bringup 1)
pico_add_extra_outputs(bringup)
