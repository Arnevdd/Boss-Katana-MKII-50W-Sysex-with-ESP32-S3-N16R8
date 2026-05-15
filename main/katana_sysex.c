#include "katana_sysex.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

const uint8_t katana_sysex_identity_15[15] = {
    0xF0, 0x7E, 0x00, 0x06, 0x02, 0x41, 0x33, 0x03, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xF7,
};

const uint8_t katana_sysex_editor_mode_15[15] = {
    0xF0, 0x41, 0x00, 0x00, 0x00, 0x00, 0x33, 0x12, 0x7F, 0x00, 0x00, 0x01, 0x01, 0x7F, 0xF7,
};

const uint8_t katana_sysex_reverb_color[3][15] = {
    {0xF0, 0x41, 0x00, 0x00, 0x00, 0x00, 0x33, 0x12, 0x60, 0x00, 0x06, 0x3D, 0x00, 0x5D, 0xF7},
    {0xF0, 0x41, 0x00, 0x00, 0x00, 0x00, 0x33, 0x12, 0x60, 0x00, 0x06, 0x3D, 0x01, 0x5C, 0xF7},
    {0xF0, 0x41, 0x00, 0x00, 0x00, 0x00, 0x33, 0x12, 0x60, 0x00, 0x06, 0x3D, 0x02, 0x5B, 0xF7},
};

esp_err_t katana_send_handshake_sequence(void (*send_raw_sysex)(const uint8_t *data, size_t len, void *ctx), void *ctx)
{
    if (!send_raw_sysex) {
        return ESP_ERR_INVALID_ARG;
    }
    send_raw_sysex(katana_sysex_identity_15, sizeof(katana_sysex_identity_15), ctx);
    vTaskDelay(pdMS_TO_TICKS(20));
    send_raw_sysex(katana_sysex_identity_15, sizeof(katana_sysex_identity_15), ctx);
    vTaskDelay(pdMS_TO_TICKS(20));
    send_raw_sysex(katana_sysex_editor_mode_15, sizeof(katana_sysex_editor_mode_15), ctx);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t katana_send_reverb_color(void (*send_raw_sysex)(const uint8_t *data, size_t len, void *ctx), void *ctx,
                                   unsigned color_index)
{
    if (!send_raw_sysex || color_index > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    send_raw_sysex(katana_sysex_reverb_color[color_index], sizeof(katana_sysex_reverb_color[color_index]), ctx);
    return ESP_OK;
}
