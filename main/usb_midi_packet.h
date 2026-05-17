#pragma once

#include <stddef.h>
#include <stdint.h>

int usb_midi_pack_sysex(uint8_t cable, const uint8_t *sysex, size_t sysex_len,
                        uint8_t *out, size_t out_cap, size_t *out_len);

int usb_midi_pack_program_change(uint8_t cable, uint8_t midi_channel, uint8_t program,
                                 uint8_t out[4]);
