
#include "pwm_sound.h"

static volatile int sound_counter = 0;
static volatile int sound_frequency = 0;
static volatile int sound_duration = 0;
static int slice_l, slice_r;
static bool sound_enabled = true;

void pwm_interrupt_handler() {
    pwm_clear_irq(slice_l);
    pwm_clear_irq(slice_r);

    if (sound_duration > 0) {
        sound_counter++;
        // Generate square wave
        // Interrupt rate is approx 87.5kHz (133MHz / (6.05 * 251))
        // Half period in samples = (87500 / frequency) / 2
        int half_period = (87500 / (sound_frequency * 2));
        if (half_period < 1) half_period = 1;

        if ((sound_counter / half_period) % 2 == 0) {
            pwm_set_chan_level(slice_l, PWM_CHAN_A, 125); // 50% duty
            pwm_set_chan_level(slice_r, PWM_CHAN_B, 125);
        } else {
            pwm_set_chan_level(slice_l, PWM_CHAN_A, 0);
            pwm_set_chan_level(slice_r, PWM_CHAN_B, 0);
        }

        sound_duration--;
    } else {
        pwm_set_chan_level(slice_l, PWM_CHAN_A, 0);
        pwm_set_chan_level(slice_r, PWM_CHAN_B, 0);
    }
}

void sound_init() {
    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);

    slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
    slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);

    pwm_clear_irq(slice_l);
    pwm_set_irq_enabled(slice_l, true);

    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 6.05f); // 133MHz
    pwm_config_set_wrap(&config, 250);

    pwm_init(slice_l, &config, true);
    pwm_init(slice_r, &config, true);

    pwm_set_chan_level(slice_l, PWM_CHAN_A, 0);
    pwm_set_chan_level(slice_r, PWM_CHAN_B, 0);
}

void sound_play(sound_type_t snd) {
    if (!sound_enabled) return;
    switch(snd) {
        case SND_BEEP:
            sound_frequency = 1000;
            sound_duration = 8750; // 100ms
            break;
        case SND_TAB_SWITCH:
            sound_frequency = 1500;
            sound_duration = 4375; // 50ms
            break;
        case SND_ERROR:
            sound_frequency = 400;
            sound_duration = 17500; // 200ms
            break;
    }
    sound_counter = 0;
}

void sound_set_enabled(bool enabled) {
    sound_enabled = enabled;
}

bool sound_is_enabled() {
    return sound_enabled;
}
