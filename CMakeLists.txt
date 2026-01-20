cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(coyote C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

include_directories(
  ${CMAKE_CURRENT_LIST_DIR}
   ${CMAKE_CURRENT_LIST_DIR}/gifenc
)

add_subdirectory(i2ckbd)
add_subdirectory(lcdspi)
add_subdirectory(pwm_sound)
add_subdirectory(pico-vfs)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/rp2040-psram rp2040-psram)

add_executable(coyote
	main.c
        utils.c
         gifenc/gifenc.c
         gif_utils.c
        UI/ui.c
        text_mode.c
        keyboard_definition.h
        tinyexpr/tinyexpr.c
        config.h
)

##picocalc spi0
target_compile_definitions(coyote PRIVATE
    PSRAM_MUTEX=1
    #PSRAM_SPINLOCK=0
    PSRAM_ASYNC=0
    PSRAM_PIN_CS=20
    PSRAM_PIN_SCK=21
    PSRAM_PIN_MOSI=2
    PSRAM_PIN_MISO=3
)

target_link_libraries(coyote PRIVATE
    pico_stdlib
    hardware_flash
    hardware_irq
    hardware_adc
    hardware_pwm
    hardware_i2c
    hardware_spi
    hardware_dma
    hardware_exception
	hardware_pio
    pico_multicore
	i2ckbd
	lcdspi
    rp2040-psram
	pwm_sound
    blockdevice_sd
    filesystem_fat
    filesystem_vfs
	)


pico_enable_stdio_usb(coyote 0)
pico_enable_stdio_uart(coyote 1)

pico_add_extra_outputs(coyote)
