#pragma once

#include <stdint.h>

// PWM audio on GPIO26/27 (PWM5 A/B) into the jack and the PAM8003.
void audio_init(void);
// Amplifier volume, 0-100 (PWM duty on AMP_VOL, GPIO34).
void audio_set_volume(int percent);
// A square-wave tone on both channels; returns at once, stops by itself.
void audio_tone(uint32_t hz, uint32_t ms);
