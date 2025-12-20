
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)

project(pwm_sound
        VERSION 0.0.1
        DESCRIPTION "pwm_sound for rp2040."
        )

add_library(pwm_sound INTERFACE)

target_sources(pwm_sound INTERFACE
        pwm_sound.c
        )

target_link_libraries(pwm_sound INTERFACE  pico_stdlib hardware_pwm)

target_include_directories(pwm_sound INTERFACE ${CMAKE_CURRENT_LIST_DIR})