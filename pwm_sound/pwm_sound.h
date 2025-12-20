
#ifndef PWM_SOUND_H
#define PWM_SOUND_H

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include <stdio.h>

#define PWM_CLOCK_KHZ 133000
#define AUDIO_PIN_L 26
#define AUDIO_PIN_R 27

void init_pwm(irq_handler_t);

#endif //PWM_SOUND_H
