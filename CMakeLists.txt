cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(cap_meter C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(cap_meter main.cpp)
pico_generate_pio_header(cap_meter ${CMAKE_CURRENT_LIST_DIR}/cap_timer.pio)

target_link_libraries(cap_meter pico_stdlib pico_flash hardware_adc hardware_clocks hardware_flash hardware_pio)

pico_enable_stdio_usb(cap_meter 1)
pico_enable_stdio_uart(cap_meter 0)
pico_add_extra_outputs(cap_meter)
