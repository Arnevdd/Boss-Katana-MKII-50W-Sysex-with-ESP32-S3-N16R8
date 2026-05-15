#include "usb_midi_host.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"

#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usb_midi_packet.h"

static const char *TAG = "usb_midi_host";

#define MIDI_USB_CLASS_AUDIO        0x01u
#define MIDI_USB_SUBCLASS_STREAMING 0x03u
/* BOSS KATANA USB: MIDI bulk pair is under vendor class 0xFF / subclass 0x03 (not USB Audio 0x01). */
#define ROLAND_MIDI_VENDOR_CLASS      0xFFu
#define ROLAND_MIDI_VENDOR_SUBCLASS   0x03u
#define MIDI_TX_BUF                 512

static bool interface_is_midi_bulk_family(const usb_intf_desc_t *intf)
{
    if (intf->bInterfaceClass == MIDI_USB_CLASS_AUDIO && intf->bInterfaceSubClass == MIDI_USB_SUBCLASS_STREAMING) {
        return true;
    }
    if (intf->bInterfaceClass == ROLAND_MIDI_VENDOR_CLASS && intf->bInterfaceSubClass == ROLAND_MIDI_VENDOR_SUBCLASS) {
        return true;
    }
    return false;
}

typedef struct {
    SemaphoreHandle_t tx_done;
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    usb_transfer_t *tx;
    usb_transfer_t *rx;
    uint8_t ep_out;
    uint8_t ep_in;
    uint8_t if_num;
    uint8_t if_alt;
    bool iface_claimed;
    bool ready;
    bool rx_running;
} midi_host_ctx_t;

static midi_host_ctx_t s_midi;
static SemaphoreHandle_t s_state_mu;
static QueueHandle_t s_evt_q;

typedef enum {
    USBH_EVT_NEW_ADDR = 0,
    USBH_EVT_DEV_GONE,
} usbh_evt_type_t;

typedef struct {
    usbh_evt_type_t type;
    uint8_t dev_addr;
    usb_device_handle_t gone_hdl;
} usbh_evt_t;

static void tx_done_cb(usb_transfer_t *transfer)
{
    if (transfer->context) {
        xSemaphoreGive((SemaphoreHandle_t)transfer->context);
    }
}

static void rx_dummy_cb(usb_transfer_t *transfer)
{
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED && transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGD(TAG, "RX status=%d", (int)transfer->status);
    }

    bool resubmit = false;
    if (xSemaphoreTake(s_state_mu, pdMS_TO_TICKS(2)) == pdTRUE) {
        resubmit = s_midi.ready && s_midi.rx_running && s_midi.dev && (transfer->device_handle == s_midi.dev);
        xSemaphoreGive(s_state_mu);
    }

    if (resubmit) {
        transfer->num_bytes = transfer->data_buffer_size;
        if (usb_host_transfer_submit(transfer) != ESP_OK) {
            ESP_LOGW(TAG, "RX resubmit failed");
        }
    }
}

static bool find_midi_bulk_endpoints(const usb_config_desc_t *cfg, uint8_t *out_if_num, uint8_t *out_if_alt,
                                     uint8_t *out_ep_out, uint8_t *out_ep_in)
{
    const uint16_t w_total = cfg->wTotalLength;
    int offset = 0;
    const usb_standard_desc_t *desc = (const usb_standard_desc_t *)cfg;

    *out_ep_out = 0;
    *out_ep_in = 0;

    while (desc != NULL && offset < (int)w_total) {
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
            if (interface_is_midi_bulk_family(intf)) {
                /* Alternate 0 is often a zero-bandwidth placeholder; real bulk EPs appear on a higher alternate. */
                if (intf->bNumEndpoints == 0) {
                    ESP_LOGD(TAG, "MIDI if=%u alt=%u: skip (no endpoints)", intf->bInterfaceNumber,
                             intf->bAlternateSetting);
                    desc = usb_parse_next_descriptor(desc, w_total, &offset);
                    continue;
                }

                uint8_t cand_out = 0;
                uint8_t cand_in = 0;
                for (int ei = 0; ei < intf->bNumEndpoints; ei++) {
                    int ep_off = offset;
                    const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, ei, w_total, &ep_off);
                    if (ep == NULL) {
                        continue;
                    }
                    const bool bulk = (USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_BULK);
                    const bool out_ep = (USB_EP_DESC_GET_EP_DIR(ep) == 0);
                    if (bulk && out_ep && cand_out == 0) {
                        cand_out = ep->bEndpointAddress;
                    }
                    if (bulk && !out_ep && cand_in == 0) {
                        cand_in = ep->bEndpointAddress;
                    }
                }

                if (cand_out != 0) {
                    *out_if_num = intf->bInterfaceNumber;
                    *out_if_alt = intf->bAlternateSetting;
                    *out_ep_out = cand_out;
                    *out_ep_in = cand_in;
                    const char *kind =
                        (intf->bInterfaceClass == MIDI_USB_CLASS_AUDIO) ? "UAC MIDI streaming" : "vendor MIDI (Roland-style)";
                    ESP_LOGI(TAG, "%s: if=%u alt=%u bulk OUT=0x%02X IN=0x%02X", kind, *out_if_num, *out_if_alt,
                             *out_ep_out, *out_ep_in);
                    return true;
                }

                ESP_LOGD(TAG, "MIDI if=%u alt=%u: no bulk OUT", intf->bInterfaceNumber, intf->bAlternateSetting);
            }
        }
        desc = usb_parse_next_descriptor(desc, w_total, &offset);
    }
    return false;
}

static esp_err_t start_rx_drain(void)
{
    if (s_midi.ep_in == 0 || s_midi.rx != NULL) {
        return ESP_OK;
    }
    int off = 0;
    const usb_config_desc_t *cfg = NULL;
    ESP_RETURN_ON_ERROR(usb_host_get_active_config_descriptor(s_midi.dev, &cfg), TAG, "get cfg");
    const usb_ep_desc_t *epin =
        usb_parse_endpoint_descriptor_by_address(cfg, s_midi.if_num, s_midi.if_alt, s_midi.ep_in, &off);
    if (epin == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const int mps = USB_EP_DESC_GET_MPS(epin);
    const int buf_len = usb_round_up_to_mps(mps, mps);

    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(buf_len, 0, &s_midi.rx), TAG, "alloc rx");
    s_midi.rx->device_handle = s_midi.dev;
    s_midi.rx->bEndpointAddress = s_midi.ep_in;
    s_midi.rx->callback = rx_dummy_cb;
    s_midi.rx->context = NULL;
    s_midi.rx->num_bytes = buf_len;
    s_midi.rx_running = true;
    ESP_RETURN_ON_ERROR(usb_host_transfer_submit(s_midi.rx), TAG, "submit rx");
    return ESP_OK;
}

static void close_device_locked(void)
{
    s_midi.ready = false;
    s_midi.rx_running = false;

    if (s_midi.rx) {
        usb_host_transfer_free(s_midi.rx);
        s_midi.rx = NULL;
    }
    if (s_midi.tx) {
        usb_host_transfer_free(s_midi.tx);
        s_midi.tx = NULL;
    }

    if (s_midi.dev) {
        if (s_midi.iface_claimed) {
            usb_host_interface_release(s_midi.client, s_midi.dev, s_midi.if_num);
            s_midi.iface_claimed = false;
        }
        usb_host_device_close(s_midi.client, s_midi.dev);
        s_midi.dev = NULL;
    }
    s_midi.ep_out = 0;
    s_midi.ep_in = 0;
}

static void handle_dev_gone(usb_device_handle_t gone_hdl)
{
    xSemaphoreTake(s_state_mu, portMAX_DELAY);
    if (s_midi.dev && s_midi.dev == gone_hdl) {
        ESP_LOGW(TAG, "Device gone");
        close_device_locked();
    }
    xSemaphoreGive(s_state_mu);
}

static void try_attach_new_device(uint8_t dev_addr)
{
    xSemaphoreTake(s_state_mu, portMAX_DELAY);

    if (s_midi.ready) {
        ESP_LOGW(TAG, "Already connected; ignoring new device addr=%u", dev_addr);
        xSemaphoreGive(s_state_mu);
        return;
    }

    usb_device_handle_t dev = NULL;
    esp_err_t err = usb_host_device_open(s_midi.client, dev_addr, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_device_open(%u) failed: %s", dev_addr, esp_err_to_name(err));
        xSemaphoreGive(s_state_mu);
        return;
    }

    const usb_device_desc_t *dd = NULL;
    err = usb_host_get_device_descriptor(dev, &dd);
    if (err != ESP_OK || dd == NULL) {
        ESP_LOGE(TAG, "get_device_descriptor failed");
        usb_host_device_close(s_midi.client, dev);
        xSemaphoreGive(s_state_mu);
        return;
    }

    const uint16_t vid = dd->idVendor;
    const uint16_t pid = dd->idProduct;
    ESP_LOGI(TAG, "New device VID=%04X PID=%04X", vid, pid);

    if (vid != CONFIG_KATANA_USB_VID) {
        ESP_LOGW(TAG, "VID mismatch (expected %04X)", CONFIG_KATANA_USB_VID);
        usb_host_device_close(s_midi.client, dev);
        xSemaphoreGive(s_state_mu);
        return;
    }

#if CONFIG_KATANA_ACCEPT_ANY_ROLAND_PID
    (void)pid;
#else
    if (pid != CONFIG_KATANA_USB_PID) {
        ESP_LOGW(TAG, "PID mismatch (expected %04X); enable CONFIG_KATANA_ACCEPT_ANY_ROLAND_PID to accept any Roland PID",
                 CONFIG_KATANA_USB_PID);
        usb_host_device_close(s_midi.client, dev);
        xSemaphoreGive(s_state_mu);
        return;
    }
#endif

    const usb_config_desc_t *cfg = NULL;
    err = usb_host_get_active_config_descriptor(dev, &cfg);
    if (err != ESP_OK || cfg == NULL) {
        ESP_LOGE(TAG, "get_active_config_descriptor failed");
        usb_host_device_close(s_midi.client, dev);
        xSemaphoreGive(s_state_mu);
        return;
    }

    uint8_t ifn = 0, alt = 0, ep_out = 0, ep_in = 0;
    if (!find_midi_bulk_endpoints(cfg, &ifn, &alt, &ep_out, &ep_in)) {
        ESP_LOGW(TAG, "No USB-MIDI bulk OUT interface found");
        usb_host_device_close(s_midi.client, dev);
        xSemaphoreGive(s_state_mu);
        return;
    }

    err = usb_host_interface_claim(s_midi.client, dev, ifn, alt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_interface_claim failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_midi.client, dev);
        xSemaphoreGive(s_state_mu);
        return;
    }

    err = usb_host_transfer_alloc(MIDI_TX_BUF, 0, &s_midi.tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tx alloc failed");
        usb_host_interface_release(s_midi.client, dev, ifn);
        usb_host_device_close(s_midi.client, dev);
        xSemaphoreGive(s_state_mu);
        return;
    }

    s_midi.dev = dev;
    s_midi.if_num = ifn;
    s_midi.if_alt = alt;
    s_midi.ep_out = ep_out;
    s_midi.ep_in = ep_in;
    s_midi.iface_claimed = true;

    s_midi.tx->device_handle = dev;
    s_midi.tx->bEndpointAddress = ep_out;
    s_midi.tx->callback = tx_done_cb;
    s_midi.tx->context = s_midi.tx_done;

    if (start_rx_drain() != ESP_OK) {
        ESP_LOGW(TAG, "Bulk IN drain not started (optional)");
    }

    s_midi.ready = true;
    ESP_LOGI(TAG, "MIDI host ready (if=%u alt=%u ep_out=0x%02X ep_in=0x%02X)", ifn, alt, ep_out, ep_in);
    xSemaphoreGive(s_state_mu);
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)arg;
    if (s_evt_q == NULL) {
        return;
    }
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        const usbh_evt_t e = {.type = USBH_EVT_NEW_ADDR, .dev_addr = event_msg->new_dev.address, .gone_hdl = NULL};
        if (xQueueSend(s_evt_q, &e, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "evt queue full (NEW_DEV)");
        }
    } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        const usbh_evt_t e = {.type = USBH_EVT_DEV_GONE, .dev_addr = 0, .gone_hdl = event_msg->dev_gone.dev_hdl};
        if (xQueueSend(s_evt_q, &e, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "evt queue full (DEV_GONE)");
        }
    }
}

static void midi_attach_worker(void *arg)
{
    (void)arg;
    for (;;) {
        usbh_evt_t e;
        if (xQueueReceive(s_evt_q, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (e.type == USBH_EVT_NEW_ADDR) {
            try_attach_new_device(e.dev_addr);
        } else if (e.type == USBH_EVT_DEV_GONE) {
            handle_dev_gone(e.gone_hdl);
        }
    }
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t ev = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &ev);
        if (ev & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "USB host: no clients");
        }
        if (ev & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB host: all devices freed");
        }
    }
}

static void midi_client_task(void *arg)
{
    (void)arg;
    for (;;) {
        usb_host_client_handle_events(s_midi.client, portMAX_DELAY);
    }
}

esp_err_t midi_host_start(void)
{
    memset(&s_midi, 0, sizeof(s_midi));

    s_state_mu = xSemaphoreCreateMutex();
    s_midi.tx_done = xSemaphoreCreateBinary();
    s_evt_q = xQueueCreate(8, sizeof(usbh_evt_t));
    if (!s_state_mu || !s_midi.tx_done || !s_evt_q) {
        return ESP_ERR_NO_MEM;
    }

    /* ESP-IDF v5.3.1 usb_host_config_t has no root_port_unpowered (added in later IDF). */
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");

    const usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_RETURN_ON_ERROR(usb_host_client_register(&client_cfg, &s_midi.client), TAG, "client_register");

    if (xTaskCreate(midi_attach_worker, "usb_midi_attach", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    if (xTaskCreate(usb_lib_task, "usb_events", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    if (xTaskCreate(midi_client_task, "midi_usb", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool midi_host_is_ready(void)
{
    bool r = false;
    if (xSemaphoreTake(s_state_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
        r = s_midi.ready;
        xSemaphoreGive(s_state_mu);
    }
    return r;
}

bool midi_host_send_sysex(const uint8_t *msg, size_t len, TickType_t wait)
{
    if (!msg || len == 0) {
        return false;
    }

    xSemaphoreTake(s_state_mu, portMAX_DELAY);
    if (!s_midi.ready || !s_midi.tx || !s_midi.dev) {
        xSemaphoreGive(s_state_mu);
        return false;
    }

    uint8_t *buf = s_midi.tx->data_buffer;
    const size_t cap = s_midi.tx->data_buffer_size;
    xSemaphoreGive(s_state_mu);

    size_t packed = 0;
    if (usb_midi_pack_sysex(0, msg, len, buf, cap, &packed) != 0) {
        ESP_LOGE(TAG, "USB-MIDI pack failed");
        return false;
    }

    xSemaphoreTake(s_state_mu, portMAX_DELAY);
    if (!s_midi.ready || !s_midi.tx || !s_midi.dev) {
        xSemaphoreGive(s_state_mu);
        return false;
    }

    usb_transfer_t *t = s_midi.tx;
    t->num_bytes = (int)packed;
    t->device_handle = s_midi.dev;
    t->bEndpointAddress = s_midi.ep_out;
    t->callback = tx_done_cb;
    t->context = s_midi.tx_done;
    t->flags = 0;

    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s_midi.dev, &cfg) != ESP_OK || cfg == NULL) {
        xSemaphoreGive(s_state_mu);
        return false;
    }
    int off = 0;
    const usb_ep_desc_t *ep =
        usb_parse_endpoint_descriptor_by_address(cfg, s_midi.if_num, s_midi.if_alt, s_midi.ep_out, &off);
    if (ep) {
        const int mps = USB_EP_DESC_GET_MPS(ep);
        if (mps > 0 && (packed % (size_t)mps) == 0) {
            t->flags |= USB_TRANSFER_FLAG_ZERO_PACK;
        }
    }

    (void)xSemaphoreTake(s_midi.tx_done, 0);

    const esp_err_t err = usb_host_transfer_submit(t);
    xSemaphoreGive(s_state_mu);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "submit failed: %s", esp_err_to_name(err));
        return false;
    }

    if (xSemaphoreTake(s_midi.tx_done, wait) != pdTRUE) {
        ESP_LOGE(TAG, "TX timeout");
        return false;
    }

    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE(TAG, "TX status=%d", (int)t->status);
        return false;
    }
    return true;
}
