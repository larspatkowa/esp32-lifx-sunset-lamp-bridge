#include "sunset_lamp_ble.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rmaker_standard_params.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "sunset-lamp-ble";

#define LAMP_NAME "IOTBT90A"
#define LAMP_SERVICE_UUID 0xFFFF
#define LAMP_WRITE_UUID 0xFF01
#define LAMP_NOTIFY_UUID 0xFF02
#define LAMP_CCCD_UUID 0x2902
#define LAMP_REQUIRED_MTU 25
#define LAMP_PREFERRED_MTU 64

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static struct ble_npl_callout s_command_callout;
static bool s_started;
static bool s_host_synced;
static bool s_scan_running;
static bool s_gatt_ready;
static uint8_t s_own_addr_type;
static uint8_t s_sequence;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_service_start;
static uint16_t s_service_end;
static uint16_t s_write_handle;
static uint16_t s_notify_handle;
static uint16_t s_cccd_handle;
static bool s_power;
static bool s_power_dirty;
static bool s_color_dirty;
static int s_brightness = 45;
static int s_hue = 24;
static int s_saturation = 100;
static esp_rmaker_param_t *s_power_param;

static void start_scan(void);
static void schedule_command(uint32_t delay_ms);
static void send_state_query(void);

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static bool advertised_name_matches(const uint8_t *name, uint8_t name_len)
{
    const size_t target_len = sizeof(LAMP_NAME) - 1;
    if (name == NULL || name_len < target_len) {
        return false;
    }

    for (size_t start = 0; start <= (size_t)name_len - target_len; start++) {
        size_t i = 0;
        for (; i < target_len; i++) {
            if (toupper((unsigned char)name[start + i]) != LAMP_NAME[i]) {
                break;
            }
        }
        if (i == target_len) {
            return true;
        }
    }
    return false;
}

static bool send_payload(const uint8_t *payload, uint8_t payload_len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_write_handle == 0) {
        return false;
    }

    uint8_t packet[32];
    if ((size_t)payload_len + 8 > sizeof(packet)) {
        ESP_LOGE(TAG, "BLE command is too large: %u bytes", payload_len);
        return false;
    }

    const uint16_t mtu = ble_att_mtu(s_conn_handle);
    if (mtu < (uint16_t)(payload_len + 11)) {
        ESP_LOGW(TAG, "Lamp MTU %u is too small for %u-byte command", mtu, payload_len + 8);
        return false;
    }

    s_sequence++;
    packet[0] = 0x00;
    packet[1] = s_sequence;
    packet[2] = 0x80;
    packet[3] = 0x00;
    packet[4] = 0x00;
    packet[5] = payload_len;
    packet[6] = (uint8_t)(payload_len + 1);
    packet[7] = 0x0A;
    memcpy(&packet[8], payload, payload_len);

    const int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle,
                                                packet, payload_len + 8);
    if (rc != 0) {
        ESP_LOGW(TAG, "Lamp BLE write could not be queued: %d", rc);
        return false;
    }
    ESP_LOGI(TAG, "Queued lamp BLE opcode 0x%02X (%u-byte payload, MTU %u)",
             payload[0], payload_len, mtu);
    return true;
}

static uint8_t make_color_payload(uint8_t payload[14])
{
    int brightness;
    int hue;
    int saturation;
    portENTER_CRITICAL(&s_state_lock);
    brightness = s_brightness;
    hue = s_hue;
    saturation = s_saturation;
    portEXIT_CRITICAL(&s_state_lock);

    const uint16_t packed_hue_saturation =
        (uint16_t)((clamp_int(hue, 0, 360) << 7) | clamp_int(saturation, 0, 100));

    payload[0] = 0xE0;
    payload[1] = 0x01;
    payload[2] = 0x00;
    payload[3] = 0xA1;
    payload[4] = (uint8_t)(packed_hue_saturation >> 8);
    payload[5] = (uint8_t)packed_hue_saturation;
    payload[6] = (uint8_t)clamp_int(brightness, 1, 100);
    payload[7] = 0x00;
    payload[8] = 0x00;
    payload[9] = 0x00;
    payload[10] = 0x00;
    payload[11] = 0x14;
    payload[12] = 0x00;
    payload[13] = 0x00;
    return 14;
}

static void command_callout(struct ble_npl_event *event)
{
    (void)event;
    if (!s_gatt_ready || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    bool send_power;
    bool power;
    bool send_color;
    portENTER_CRITICAL(&s_state_lock);
    send_power = s_power_dirty;
    power = s_power;
    if (send_power) {
        s_power_dirty = false;
    }
    send_color = s_color_dirty;
    if (send_color) {
        s_color_dirty = false;
    }
    portEXIT_CRITICAL(&s_state_lock);

    bool power_sent = !send_power;
    if (send_power) {
        const uint8_t payload[] = {0x71, power ? 0x23 : 0x24};
        power_sent = send_payload(payload, sizeof(payload));
    }

    bool color_sent = !send_color;
    if (send_color) {
        uint8_t payload[14];
        const uint8_t payload_len = make_color_payload(payload);
        const uint16_t hue_saturation = ((uint16_t)payload[4] << 8) | payload[5];
        ESP_LOGI(TAG, "Lamp color brightness=%u packed_hue_saturation=0x%04X",
                 payload[6], hue_saturation);
        color_sent = send_payload(payload, payload_len);
    }

    portENTER_CRITICAL(&s_state_lock);
    if (send_power && !power_sent) {
        s_power_dirty = true;
    }
    if (send_color && !color_sent) {
        s_color_dirty = true;
    }
    const bool needs_retry = s_power_dirty || s_color_dirty;
    portEXIT_CRITICAL(&s_state_lock);

    if (needs_retry) {
        schedule_command(100);
    }
}

static void schedule_command(uint32_t delay_ms)
{
    if (s_started) {
        ble_npl_callout_reset(&s_command_callout,
                              ble_npl_time_ms_to_ticks32(delay_ms));
    }
}

static void report_power(bool is_on)
{
    bool changed;
    portENTER_CRITICAL(&s_state_lock);
    changed = s_power != is_on;
    s_power = is_on;
    portEXIT_CRITICAL(&s_state_lock);

    if (changed && s_power_param != NULL) {
        esp_rmaker_param_update_and_report(s_power_param, esp_rmaker_bool(is_on));
    }
}

static void consume_lamp_notification(struct os_mbuf *om)
{
    const uint16_t packet_len = OS_MBUF_PKTLEN(om);
    uint8_t packet[32];
    if (packet_len < 15 || packet_len > sizeof(packet)) {
        return;
    }
    if (os_mbuf_copydata(om, 0, packet_len, packet) != 0) {
        return;
    }
    if (packet[8] != 0xEA || packet[9] != 0x81) {
        return;
    }

    if (packet[14] == 0x23) {
        report_power(true);
    } else if (packet[14] == 0x24) {
        report_power(false);
    }
}

static void send_state_query(void)
{
    static const uint8_t query[] = {0xEA, 0x81, 0x8A, 0x8B};
    send_payload(query, sizeof(query));
}

static int subscribe_complete(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr,
                              void *arg)
{
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "Notification subscription failed: %d", error->status);
    }
    if (conn_handle == s_conn_handle) {
        s_gatt_ready = true;
        send_state_query();
        schedule_command(100);
    }
    return 0;
}

static int descriptor_discovered(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 uint16_t chr_val_handle,
                                 const struct ble_gatt_dsc *descriptor,
                                 void *arg)
{
    (void)chr_val_handle;
    (void)arg;

    if (error->status == 0 && descriptor != NULL &&
        ble_uuid_u16(&descriptor->uuid.u) == LAMP_CCCD_UUID) {
        s_cccd_handle = descriptor->handle;
        const uint8_t enable_notifications[] = {0x01, 0x00};
        const int rc = ble_gattc_write_flat(conn_handle, s_cccd_handle,
                                             enable_notifications,
                                             sizeof(enable_notifications),
                                             subscribe_complete, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Could not enable lamp notifications: %d", rc);
            s_gatt_ready = true;
            send_state_query();
            schedule_command(100);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Lamp has no notification descriptor; using write-only control");
        s_gatt_ready = true;
        send_state_query();
        schedule_command(100);
    }
    return 0;
}

static int characteristic_discovered(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *characteristic,
                                     void *arg)
{
    (void)arg;
    if (error->status == 0 && characteristic != NULL) {
        const uint16_t uuid = ble_uuid_u16(&characteristic->uuid.u);
        ESP_LOGI(TAG, "Lamp characteristic 0x%04X handle=%u properties=0x%02X",
                 uuid, characteristic->val_handle, characteristic->properties);
        if (uuid == LAMP_WRITE_UUID) {
            s_write_handle = characteristic->val_handle;
        } else if (uuid == LAMP_NOTIFY_UUID) {
            s_notify_handle = characteristic->val_handle;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_write_handle == 0 || s_notify_handle == 0) {
            ESP_LOGE(TAG, "Lamp service is missing FF01/FF02 characteristics");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (ble_att_mtu(conn_handle) < LAMP_REQUIRED_MTU) {
            ESP_LOGE(TAG, "Lamp negotiated an MTU too small for color commands");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        const int rc = ble_gattc_disc_all_dscs(conn_handle, s_notify_handle,
                                                s_service_end,
                                                descriptor_discovered, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Could not inspect lamp notification descriptors: %d", rc);
            s_gatt_ready = true;
            send_state_query();
            schedule_command(100);
        }
    } else if (error->status != 0) {
        ESP_LOGE(TAG, "Lamp characteristic discovery failed: %d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int service_discovered(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *arg)
{
    (void)arg;
    if (error->status == 0 && service != NULL) {
        if (ble_uuid_u16(&service->uuid.u) == LAMP_SERVICE_UUID) {
            s_service_start = service->start_handle;
            s_service_end = service->end_handle;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_service_start == 0 || s_service_end == 0) {
            ESP_LOGE(TAG, "Lamp does not expose the expected FFFF service");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        const int rc = ble_gattc_disc_all_chrs(conn_handle, s_service_start,
                                                s_service_end,
                                                characteristic_discovered, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Could not inspect lamp characteristics: %d", rc);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    } else if (error->status != 0) {
        ESP_LOGE(TAG, "Lamp service discovery failed: %d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int mtu_exchanged(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         uint16_t mtu,
                         void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Lamp ATT MTU: %u", mtu);
    if (error->status != 0) {
        ESP_LOGW(TAG, "MTU exchange did not complete: %d", error->status);
    }
    if (ble_att_mtu(conn_handle) < LAMP_REQUIRED_MTU) {
        ESP_LOGE(TAG, "Lamp ATT MTU is too small for its 22-byte command frames");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    const int rc = ble_gattc_disc_all_svcs(conn_handle, service_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not inspect lamp services: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) != 0 ||
            !advertised_name_matches(fields.name, fields.name_len)) {
            return 0;
        }

        ESP_LOGI(TAG, "Found %s at RSSI %d", LAMP_NAME, event->disc.rssi);
        ble_gap_disc_cancel();
        s_scan_running = false;
        const int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr,
                                       30000, NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Could not connect to lamp: %d", rc);
            start_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Lamp connection failed: %d", event->connect.status);
            start_scan();
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        s_service_start = 0;
        s_service_end = 0;
        s_write_handle = 0;
        s_notify_handle = 0;
        s_cccd_handle = 0;
        s_gatt_ready = false;
        ESP_LOGI(TAG, "Connected to sunset lamp; discovering GATT");
        if (ble_gattc_exchange_mtu(s_conn_handle, mtu_exchanged, NULL) != 0) {
            ESP_LOGW(TAG, "Could not request a larger MTU");
            mtu_exchanged(s_conn_handle, &(struct ble_gatt_error){.status = 0},
                          ble_att_mtu(s_conn_handle), NULL);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Lamp disconnected; scanning again");
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_service_start = 0;
        s_service_end = 0;
        s_write_handle = 0;
        s_notify_handle = 0;
        s_cccd_handle = 0;
        s_gatt_ready = false;
        start_scan();
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == s_conn_handle && event->notify_rx.om != NULL) {
            consume_lamp_notification(event->notify_rx.om);
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scan_running = false;
        start_scan();
        return 0;

    default:
        return 0;
    }
}

static void start_scan(void)
{
    if (!s_host_synced || s_scan_running ||
        s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct ble_gap_disc_params params = {
        .itvl = 0x0050,
        .window = 0x0030,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 1,
    };
    const int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params,
                                gap_event, NULL);
    if (rc == 0) {
        s_scan_running = true;
        ESP_LOGI(TAG, "Scanning for %s", LAMP_NAME);
    } else {
        ESP_LOGW(TAG, "BLE scan could not start: %d", rc);
    }
}

static void ble_host_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset: %d", reason);
    s_host_synced = false;
    s_scan_running = false;
    s_gatt_ready = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static void ble_host_sync(void)
{
    const int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not determine BLE address type: %d", rc);
        return;
    }
    s_host_synced = true;
    start_scan();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t sunset_lamp_ble_start(esp_rmaker_param_t *power_param)
{
    if (s_started) {
        return ESP_OK;
    }

    s_power_param = power_param;
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start NimBLE after RainMaker provisioning: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(ble_att_set_preferred_mtu(LAMP_PREFERRED_MTU));
    ble_hs_cfg.reset_cb = ble_host_reset;
    ble_hs_cfg.sync_cb = ble_host_sync;
    /* The lamp does not pair, so no persistent bond store is needed. */

    const int callout_rc = ble_npl_callout_init(&s_command_callout,
                                                nimble_port_get_dflt_eventq(),
                                                command_callout, NULL);
    if (callout_rc != 0) {
        ESP_LOGE(TAG, "Could not create the lamp command queue: %d", callout_rc);
        return ESP_FAIL;
    }

    s_started = true;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

void sunset_lamp_ble_set_power(bool is_on)
{
    portENTER_CRITICAL(&s_state_lock);
    s_power = is_on;
    s_power_dirty = true;
    portEXIT_CRITICAL(&s_state_lock);
    /* Let RainMaker finish applying a batched power/color update first. */
    schedule_command(100);
}

void sunset_lamp_ble_set_brightness(int brightness)
{
    portENTER_CRITICAL(&s_state_lock);
    s_brightness = clamp_int(brightness, 0, 100);
    s_color_dirty = true;
    portEXIT_CRITICAL(&s_state_lock);
    schedule_command(100);
}

void sunset_lamp_ble_set_hue(int hue)
{
    portENTER_CRITICAL(&s_state_lock);
    s_hue = clamp_int(hue, 0, 360);
    s_color_dirty = true;
    portEXIT_CRITICAL(&s_state_lock);
    schedule_command(100);
}

void sunset_lamp_ble_set_saturation(int saturation)
{
    portENTER_CRITICAL(&s_state_lock);
    s_saturation = clamp_int(saturation, 0, 100);
    s_color_dirty = true;
    portEXIT_CRITICAL(&s_state_lock);
    schedule_command(100);
}
