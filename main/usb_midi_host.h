#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t midi_host_start(void);
bool midi_host_is_ready(void);
bool midi_host_send_sysex(const uint8_t *msg, size_t len, TickType_t wait);
bool midi_host_send_program_change(uint8_t program, TickType_t wait);

#ifdef __cplusplus
}
#endif
