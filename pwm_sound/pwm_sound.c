
#include "pwm_sound.h"


void init_pwm(irq_handler_t my_handler) {

    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);

    int slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
    int slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);


    pwm_clear_irq(slice_l);
    pwm_clear_irq(slice_r);
    pwm_set_irq_enabled(slice_l, true);
    pwm_set_irq_enabled(slice_r, true);

    irq_set_exclusive_handler(PWM_IRQ_WRAP, my_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);


    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 6.05f); // 133MHz
    pwm_config_set_wrap(&config, 250);


    pwm_init(slice_l, &config, true);
    pwm_init(slice_r, &config, true);

    pwm_set_chan_level(slice_l, PWM_CHAN_A, 0);
    pwm_set_chan_level(slice_r, PWM_CHAN_B, 0);
}
