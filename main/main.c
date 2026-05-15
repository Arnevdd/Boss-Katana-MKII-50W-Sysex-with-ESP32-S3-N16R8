/*
 * ESP-IDF 5.3 — ESP32-S3 USB host → Boss Katana (USB-MIDI class on official usb_host stack).
 * USB OTG: internal PHY, default pins D− GPIO19 / D+ GPIO20 (matches many S3 boards).
 *
 * SysEx bytes: KatanaFootController docs/katana.txt + MS3-style editor handshake (see katana_sysex.c).
 */

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "katana_sysex.h"
#include "usb_midi_host.h"

static const char *TAG = "app";

#define BTN_GPIO        GPIO_NUM_4
#define BTN_DEBOUNCE_MS 40

static void katana_tx_bridge(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (!midi_host_send_sysex(data, len, pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "MIDI TX failed (len=%u)", (unsigned)len);
    }
}

static bool btn_pressed_raw(void)
{
    return gpio_get_level(BTN_GPIO) == 0;
}

static void wait_button_press(void)
{
    for (;;) {
        if (!btn_pressed_raw()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
        if (btn_pressed_raw()) {
            while (btn_pressed_raw()) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
            return;
        }
    }
}

static void katana_test_loop_task(void *arg)
{
    (void)arg;

#if CONFIG_KATANA_ACCEPT_ANY_ROLAND_PID
    ESP_LOGI(TAG, "Waiting for USB-MIDI (VID=%04X, any Roland PID)...", (unsigned)CONFIG_KATANA_USB_VID);
#else
    ESP_LOGI(TAG, "Waiting for USB-MIDI (VID=%04X PID=%04X)...", (unsigned)CONFIG_KATANA_USB_VID,
             (unsigned)CONFIG_KATANA_USB_PID);
#endif

    while (!midi_host_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "Katana USB-MIDI connected. Press GPIO4 (to GND) to run handshake + colour cycle.");
    wait_button_press();

    ESP_LOGI(TAG, "Sending handshake / editor entry...");
    if (katana_send_handshake_sequence(katana_tx_bridge, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Handshake sequence failed");
        vTaskDelete(NULL);
        return;
    }

    unsigned colour = 0;
    for (;;) {
        ESP_LOGI(TAG, "Reverb colour index %u", colour);
        if (katana_send_reverb_color(katana_tx_bridge, NULL, colour) != ESP_OK) {
            ESP_LOGE(TAG, "Reverb colour send failed");
        }
        colour = (colour + 1) % 3;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Project root: Desktop/Effectboard ESP32/katana_usb_midi_host");
    ESP_LOGI(TAG, "USB host uses ESP-IDF usb_host (not Arduino / not external TinyUSB host fork).");

    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    ESP_ERROR_CHECK(midi_host_start());

    if (xTaskCreate(katana_test_loop_task, "katana_demo", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start demo task");
    }
}
