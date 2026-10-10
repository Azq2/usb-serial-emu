cmake_minimum_required(VERSION 3.13)

if(NOT PICO_SDK_PATH)
	set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk")
endif()

if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
	message(FATAL_ERROR "Pico SDK not found: set PICO_SDK_PATH or clone it into lib/pico-sdk")
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(usb_serial_emu C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/lib/cherryusb/cherryusb.cmake")
	message(FATAL_ERROR "CherryUSB not found: initialize the lib/cherryusb submodule")
endif()

set(CONFIG_CHERRYUSB_DEVICE 1)
set(CONFIG_CHERRYUSB_DEVICE_RP2040 1)
include(${CMAKE_CURRENT_LIST_DIR}/lib/cherryusb/cherryusb.cmake)

set_source_files_properties(
	${CMAKE_CURRENT_LIST_DIR}/lib/cherryusb/core/usbd_core.c
	PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/src/cherryusb_overrides.h"
)
foreach(source IN LISTS cherryusb_srcs)
	set_property(SOURCE ${source} APPEND PROPERTY COMPILE_OPTIONS -w)
endforeach()

add_executable(usb_serial_emu
	src/main.c
	src/runtime_profile.c
	src/personality_switch.c
	src/usb_serial_dispatch.c
	src/usb_descriptors.c
	src/usb_device.c
	src/rp2040_usb_workarounds.c
	src/cdc_acm_device.c
	src/ch341_device.c
	src/cp210x_device.c
	src/pl2303_device.c
	src/ft232_device.c
	${cherryusb_srcs}
)

target_include_directories(usb_serial_emu PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/src
	${cherryusb_incs}
)

target_link_libraries(usb_serial_emu PRIVATE
	pico_stdlib
	pico_flash
	hardware_flash
	hardware_irq
	hardware_resets
	hardware_watchdog
	pico_fix_rp2040_usb_device_enumeration
)

target_link_options(usb_serial_emu PRIVATE
	-Wl,--wrap=usbd_ep_clear_stall
)

pico_enable_stdio_usb(usb_serial_emu 0)
pico_enable_stdio_uart(usb_serial_emu 0)
pico_add_extra_outputs(usb_serial_emu)

target_compile_options(usb_serial_emu PRIVATE -Wall -Wextra)
