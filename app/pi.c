#include "pi.h"
#include "reg.h"
#include "keyboard.h"
#include "gpioexp.h"
#include "backlight.h"
#include "hardware/adc.h"
#include <hardware/pwm.h>

#include <pico/stdlib.h>

void pi_power_init(void)
{
	adc_init();
	adc_gpio_init(PIN_BAT_ADC);
	adc_select_input(0);

	gpio_init(PIN_PI_PWR);
	gpio_set_dir(PIN_PI_PWR, GPIO_OUT);
}

void pi_power_on(void)
{
	gpio_put(PIN_PI_PWR, 0);
	busy_wait_us(200000);
	gpio_put(PIN_PI_PWR, 1);

	// LED green while booting until driver loaded
    reg_set_value(REG_ID_LED, 1);
    reg_set_value(REG_ID_LED_R, 0);
    reg_set_value(REG_ID_LED_G, 128);
    reg_set_value(REG_ID_LED_B, 0);
	led_sync();
}

void led_init(void)
{
    // Set up PWM channels
    gpio_set_function(PIN_LED_R, GPIO_FUNC_PWM);
    gpio_set_function(PIN_LED_G, GPIO_FUNC_PWM);
    gpio_set_function(PIN_LED_B, GPIO_FUNC_PWM);

    //default off
    reg_set_value(REG_ID_LED, 0);

    led_sync();
}

void led_sync(void){
    // Set the PWM slice for each channel
    uint slice_r = pwm_gpio_to_slice_num(PIN_LED_R);
    uint slice_g = pwm_gpio_to_slice_num(PIN_LED_G);
    uint slice_b = pwm_gpio_to_slice_num(PIN_LED_B);

    // Calculate the PWM value for each channel
    uint16_t pwm_r = (0xFF - reg_get_value(REG_ID_LED_R)) * 0x101;
    uint16_t pwm_g = (0xFF - reg_get_value(REG_ID_LED_G)) * 0x101;
    uint16_t pwm_b = (0xFF - reg_get_value(REG_ID_LED_B)) * 0x101;

    // Set the PWM duty cycle for each channel
    if(reg_get_value(REG_ID_LED) == 0){
        pwm_set_gpio_level(PIN_LED_R, 0xFFFF);
        pwm_set_gpio_level(PIN_LED_G, 0xFFFF);
        pwm_set_gpio_level(PIN_LED_B, 0xFFFF);
    } else {
        pwm_set_gpio_level(PIN_LED_R, pwm_r);
        pwm_set_gpio_level(PIN_LED_G, pwm_g);
        pwm_set_gpio_level(PIN_LED_B, pwm_b);
    }

    // Enable PWM channels
    pwm_set_enabled(slice_r, true);
    pwm_set_enabled(slice_g, true);
    pwm_set_enabled(slice_b, true);
}
