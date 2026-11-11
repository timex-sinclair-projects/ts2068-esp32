// PWM audio bring-up: square-wave tones on GPIO26/27.
//
// The amplifier (PAM8003, U11) is controlled through two transistors
// (Rev A1 netlist):
// - GPIO35 -> T4 pulls SHDN# low, so GPIO35 high = amplifier off.
// - GPIO34 (PWM9_A) -> T3 -> T2 drives the VOL pin from +5VA through R77,
//   filtered by C79, so a higher duty gives a higher VOL voltage.
// Plugging in headphones also pulls SHDN# (HP_DET), muting the speakers.

#include "audio.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

#define PWM_DIV 16

static uint audio_slice;
static alarm_id_t stop_alarm;

static void silence(void) {
    pwm_set_both_levels(audio_slice, 0, 0);
}

static int64_t stop_cb(alarm_id_t id, void *user) {
    silence();
    stop_alarm = 0;
    return 0;
}

void audio_init(void) {
    // Amplifier on.
    gpio_init(RP2350PC_AMP_SHDN_PIN);
    gpio_put(RP2350PC_AMP_SHDN_PIN, 0);
    gpio_set_dir(RP2350PC_AMP_SHDN_PIN, GPIO_OUT);

    // Volume control: PWM9_A at about 30 kHz.
    gpio_set_function(RP2350PC_AMP_VOL_PIN, GPIO_FUNC_PWM);
    uint vs = pwm_gpio_to_slice_num(RP2350PC_AMP_VOL_PIN);
    pwm_config vc = pwm_get_default_config();
    pwm_config_set_wrap(&vc, 999);
    pwm_config_set_clkdiv(&vc, (float)clock_get_hz(clk_sys) / (1000.0f * 30000.0f));
    pwm_init(vs, &vc, true);
    audio_set_volume(40);

    gpio_set_function(RP2350PC_AUDIO_PWM_L_PIN, GPIO_FUNC_PWM);
    gpio_set_function(RP2350PC_AUDIO_PWM_R_PIN, GPIO_FUNC_PWM);
    audio_slice = pwm_gpio_to_slice_num(RP2350PC_AUDIO_PWM_L_PIN);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, PWM_DIV);
    pwm_config_set_wrap(&c, 0xffff);
    pwm_init(audio_slice, &c, true);
    silence();
}

void audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    pwm_set_gpio_level(RP2350PC_AMP_VOL_PIN, (uint16_t)(percent * 10));
}

void audio_tone(uint32_t hz, uint32_t ms) {
    if (stop_alarm) {
        cancel_alarm(stop_alarm);
        stop_alarm = 0;
    }
    uint32_t wrap = clock_get_hz(clk_sys) / PWM_DIV / hz - 1;
    if (wrap > 0xffff) wrap = 0xffff;
    pwm_set_wrap(audio_slice, (uint16_t)wrap);
    // A quarter duty: quieter than half, still a clear tone.
    pwm_set_both_levels(audio_slice, (uint16_t)(wrap / 4), (uint16_t)(wrap / 4));
    stop_alarm = add_alarm_in_ms(ms, stop_cb, NULL, true);
}
