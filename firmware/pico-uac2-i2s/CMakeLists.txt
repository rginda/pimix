cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (must be before project)
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/src/pico-sdk")
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pimix_uac2_i2s C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pimix_uac2_i2s
    src/main.c
    src/usb_descriptors.c
    src/quirk_os_guessing.c
)

pico_generate_pio_header(pimix_uac2_i2s ${CMAKE_CURRENT_LIST_DIR}/src/i2s_slave.pio)

target_include_directories(pimix_uac2_i2s PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_compile_definitions(pimix_uac2_i2s PRIVATE
    CFG_TUSB_MCU=OPT_MCU_RP2040
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
)

target_link_libraries(pimix_uac2_i2s PRIVATE
    pico_stdlib
    pico_unique_id
     pico_multicore
    pico_bootrom
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_watchdog
    hardware_uart
    tinyusb_device
    tinyusb_board
)

pico_add_extra_outputs(pimix_uac2_i2s)
