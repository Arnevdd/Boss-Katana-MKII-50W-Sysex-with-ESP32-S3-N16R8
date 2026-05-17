#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef bool (*katana_sysex_send_fn)(const uint8_t *data, size_t len, void *ctx);

#define KATANA_SYSEX_FX_LEN  15
#define KATANA_SYSEX_CH_LEN  16 /* PARA_PC uses MS3 2-byte data (KatanaFootController valueSize=2) */

extern const uint8_t katana_sysex_identity_15[15];
extern const uint8_t katana_sysex_editor_mode_15[15];

typedef enum {
    KATANA_EFFECT_BOOSTER = 0,
    KATANA_EFFECT_MOD,
    KATANA_EFFECT_FX,
    KATANA_EFFECT_DELAY,
    KATANA_EFFECT_REVERB,
    KATANA_EFFECT_COUNT,
} katana_effect_t;

#define KATANA_EFFECT_CYCLE_STEPS 4

esp_err_t katana_send_handshake_sequence(katana_sysex_send_fn send_raw_sysex, void *ctx);

esp_err_t katana_send_channel(katana_sysex_send_fn send_raw_sysex, void *ctx, unsigned channel_1_to_4);

/* Program Change value for GPIO CH1–4 (must match Tone Studio PROGRAM MAP). */
uint8_t katana_channel_midi_program(unsigned channel_1_to_4);

esp_err_t katana_send_effect_cycle_step(katana_sysex_send_fn send_raw_sysex, void *ctx, katana_effect_t effect,
                                        unsigned step);
