#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define KATANA_MS3_HEADER_0 0x41
#define KATANA_MS3_HEADER_1 0x00
#define KATANA_MS3_HEADER_2 0x00
#define KATANA_MS3_HEADER_3 0x00
#define KATANA_MS3_HEADER_4 0x00
#define KATANA_MS3_HEADER_5 0x33

#define KATANA_PARA_REVERB      0x60000540UL
#define KATANA_PARA_REVERB_LED  0x60000661UL

extern const uint8_t katana_sysex_identity_15[15];
extern const uint8_t katana_sysex_editor_mode_15[15];
extern const uint8_t katana_sysex_reverb_color[3][15];

esp_err_t katana_send_handshake_sequence(void (*send_raw_sysex)(const uint8_t *data, size_t len, void *ctx), void *ctx);

esp_err_t katana_send_reverb_color(void (*send_raw_sysex)(const uint8_t *data, size_t len, void *ctx), void *ctx,
                                   unsigned color_index);
