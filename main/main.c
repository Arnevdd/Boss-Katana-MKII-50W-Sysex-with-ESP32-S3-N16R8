/*
 * ESP-IDF 5.3 — ESP32-S3 USB host → Boss Katana 50W MKII (USB-MIDI).
 * Ten momentary buttons: GPIO1–4 channel select, GPIO7–11 effect cycle.
 *
 * Buttons: internal pull-up, one side to GPIO, other to GND (idle HIGH, press = LOW).
 * Action on each press (HIGH → LOW edge). Matches typical footswitch wiring.
 */

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "katana_sysex.h"
#include "usb_midi_host.h"

static const char *TAG = "app";

#define BTN_DEBOUNCE_MS 40
#define BTN_POLL_MS     10

/* Onboard LED: GPIO48 (ESP32-S3-DevKitC-1 v1.1). Use GPIO38 on older DevKitC boards. */
#define STATUS_LED_GPIO GPIO_NUM_48

#define GPIO_CH1  GPIO_NUM_1
#define GPIO_CH2  GPIO_NUM_2
#define GPIO_CH3  GPIO_NUM_3
#define GPIO_CH4  GPIO_NUM_4

#define GPIO_BOOSTER GPIO_NUM_7
#define GPIO_MOD     GPIO_NUM_8
#define GPIO_FX      GPIO_NUM_9
#define GPIO_DELAY   GPIO_NUM_10
#define GPIO_REVERB  GPIO_NUM_11

#define BTN_MASK_CH                                                                                                    \
    ((1ULL << GPIO_CH1) | (1ULL << GPIO_CH2) | (1ULL << GPIO_CH3) | (1ULL << GPIO_CH4) | (1ULL << GPIO_BOOSTER) |     \
     (1ULL << GPIO_MOD) | (1ULL << GPIO_FX) | (1ULL << GPIO_DELAY) | (1ULL << GPIO_REVERB))

typedef struct {
    gpio_num_t gpio;
    bool last_pressed;
    uint8_t cycle_step;
    katana_effect_t effect;
    bool is_effect;
    unsigned channel_num;
    const char *label;
} button_t;

static button_t s_buttons[] = {
    {.gpio = GPIO_CH1, .label = "A-CH1", .is_effect = false, .channel_num = 1},
    {.gpio = GPIO_CH2, .label = "A-CH2", .is_effect = false, .channel_num = 2},
    {.gpio = GPIO_CH3, .label = "B-CH1", .is_effect = false, .channel_num = 3},
    {.gpio = GPIO_CH4, .label = "B-CH2", .is_effect = false, .channel_num = 4},
    {.gpio = GPIO_BOOSTER, .label = "BOOSTER", .cycle_step = 0, .effect = KATANA_EFFECT_BOOSTER, .is_effect = true},
    {.gpio = GPIO_MOD, .label = "MOD", .cycle_step = 0, .effect = KATANA_EFFECT_MOD, .is_effect = true},
    {.gpio = GPIO_FX, .label = "FX", .cycle_step = 0, .effect = KATANA_EFFECT_FX, .is_effect = true},
    {.gpio = GPIO_DELAY, .label = "DELAY", .cycle_step = 0, .effect = KATANA_EFFECT_DELAY, .is_effect = true},
    {.gpio = GPIO_REVERB, .label = "REVERB", .cycle_step = 0, .effect = KATANA_EFFECT_REVERB, .is_effect = true},
};

static bool s_handshake_done;

static bool katana_tx_bridge(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (!midi_host_send_sysex(data, len, pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "MIDI TX failed (len=%u)", (unsigned)len);
        return false;
    }
    return true;
}

static bool gpio_is_pressed(gpio_num_t gpio)
{
    return gpio_get_level(gpio) == 0;
}

static void status_led_init(void)
{
    const gpio_config_t led = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(STATUS_LED_GPIO, 1);
}

static void status_led_blink(void)
{
    gpio_set_level(STATUS_LED_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_level(STATUS_LED_GPIO, 1);
}

static void log_gpio_levels(void)
{
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++) {
        const button_t *btn = &s_buttons[i];
        ESP_LOGI(TAG, "GPIO%d (%s) level=%d (0=pressed with pull-up+GND wiring)",
                 (int)btn->gpio, btn->label, (int)gpio_get_level(btn->gpio));
    }
}

static void try_usb_handshake(void)
{
    if (!midi_host_is_ready()) {
        if (s_handshake_done) {
            ESP_LOGW(TAG, "Katana USB-MIDI disconnected");
        }
        s_handshake_done = false;
        return;
    }

    if (s_handshake_done) {
        return;
    }

    ESP_LOGI(TAG, "Katana USB-MIDI connected. Sending editor handshake...");
    if (katana_send_handshake_sequence(katana_tx_bridge, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Handshake sequence failed");
        return;
    }
    s_handshake_done = true;
    ESP_LOGI(TAG, "Handshake done — SysEx commands will be sent on button press.");
}

static const char *channel_label(unsigned channel)
{
    switch (channel) {
    case 1:
        return "bank A ch 1";
    case 2:
        return "bank A ch 2";
    case 3:
        return "bank B ch 1";
    case 4:
        return "bank B ch 2";
    default:
        return "?";
    }
}

static void on_channel_press(unsigned channel)
{
    ESP_LOGI(TAG, "SET_CH%u (%s)", channel, channel_label(channel));
    if (!midi_host_is_ready()) {
        ESP_LOGW(TAG, "Katana not connected — channel command skipped");
        return;
    }

    /* Global channel SysEx 1–4 (katana.txt), then Program Change per Tone Studio map. */
    if (katana_send_channel(katana_tx_bridge, NULL, channel) != ESP_OK) {
        ESP_LOGE(TAG, "CH%u SysEx failed", channel);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(30));
    const uint8_t pc = katana_channel_midi_program(channel);
    if (midi_host_send_program_change(pc, pdMS_TO_TICKS(2000))) {
        ESP_LOGI(TAG, "CH%u done (SysEx + PC %u)", channel, (unsigned)pc);
    } else {
        ESP_LOGI(TAG, "CH%u done (SysEx only; PC %u failed)", channel, (unsigned)pc);
    }
}

static void on_effect_press(button_t *btn)
{
    ESP_LOGI(TAG, "%s cycle step %u", btn->label, (unsigned)btn->cycle_step);
    if (!midi_host_is_ready()) {
        ESP_LOGW(TAG, "Katana not connected — %s command skipped", btn->label);
        btn->cycle_step = (uint8_t)((btn->cycle_step + 1) % KATANA_EFFECT_CYCLE_STEPS);
        return;
    }
    if (katana_send_effect_cycle_step(katana_tx_bridge, NULL, btn->effect, btn->cycle_step) != ESP_OK) {
        ESP_LOGE(TAG, "%s SysEx failed at step %u", btn->label, (unsigned)btn->cycle_step);
        return;
    }
    btn->cycle_step = (uint8_t)((btn->cycle_step + 1) % KATANA_EFFECT_CYCLE_STEPS);
}

static void handle_button_press(button_t *btn)
{
    status_led_blink();
    if (btn->is_effect) {
        on_effect_press(btn);
    } else {
        on_channel_press(btn->channel_num);
    }
}

static bool debounce_pressed(gpio_num_t gpio)
{
    if (!gpio_is_pressed(gpio)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
    return gpio_is_pressed(gpio);
}

static void katana_effectboard_task(void *arg)
{
    (void)arg;

#if CONFIG_KATANA_ACCEPT_ANY_ROLAND_PID
    ESP_LOGI(TAG, "USB-MIDI target VID=%04X (any Roland PID). Buttons work without USB; MIDI waits for Katana.",
             (unsigned)CONFIG_KATANA_USB_VID);
#else
    ESP_LOGI(TAG, "USB-MIDI target VID=%04X PID=%04X. Buttons work without USB; MIDI waits for Katana.",
             (unsigned)CONFIG_KATANA_USB_VID, (unsigned)CONFIG_KATANA_USB_PID);
#endif

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++) {
        s_buttons[i].last_pressed = gpio_is_pressed(s_buttons[i].gpio);
    }

    log_gpio_levels();
    ESP_LOGI(TAG, "Polling buttons (press = connect GPIO to GND). Watch serial + onboard LED blink.");

    for (;;) {
        try_usb_handshake();

        for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++) {
            button_t *btn = &s_buttons[i];
            const bool pressed = gpio_is_pressed(btn->gpio);

            if (!btn->last_pressed && pressed) {
                if (debounce_pressed(btn->gpio)) {
                    ESP_LOGI(TAG, "Button %s (GPIO%d)", btn->label, (int)btn->gpio);
                    handle_button_press(btn);
                }
            }

            btn->last_pressed = pressed;
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Katana USB-MIDI effectboard (ESP32-S3 USB host)");

    status_led_init();

    const gpio_config_t btn_cfg = {
        .pin_bit_mask = BTN_MASK_CH,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    ESP_ERROR_CHECK(midi_host_start());

    if (xTaskCreate(katana_effectboard_task, "katana_fxboard", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start effectboard task");
    }
}
