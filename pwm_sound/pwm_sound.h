
#ifndef PWM_SOUND_H
#define PWM_SOUND_H

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include <stdio.h>

#define PWM_CLOCK_KHZ 133000
#define AUDIO_PIN_L 26
#define AUDIO_PIN_R 27

typedef enum {
    SND_BEEP,
    SND_TAB_SWITCH,
    SND_ERROR
} sound_type_t;

void sound_init();
void sound_play(sound_type_t snd);
void sound_set_enabled(bool enabled);
bool sound_is_enabled();

#endif //PWM_SOUND_H
