#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_network.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rmaker_core.h"
#include "esp_rmaker_ota.h"
#include "esp_rmaker_standard_devices.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_types.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sunset_lamp_ble.h"
#include "private_config.h"

#define DEFAULT_LAMP_POWER false
#define DEFAULT_LAMP_BRIGHTNESS 45
#define DEFAULT_LAMP_HUE 24
#define DEFAULT_LAMP_SATURATION 100

static const char *TAG = "jelly-bridge";
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_napt_enabled;
static esp_rmaker_device_t *s_lamp_device;
static esp_rmaker_param_t *s_power_param;

static void configure_ap_dns_from_upstream(void)
{
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        return;
    }

    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read upstream DNS; bridge clients may not resolve names");
        return;
    }

    const uint8_t offer_dns = 0x02;
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           (void *)&offer_dns, sizeof(offer_dns)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Upstream connected; IP address: " IPSTR, IP2STR(&event->ip_info.ip));

    if (s_sta_netif != NULL) {
        ESP_ERROR_CHECK(esp_netif_set_default_netif(s_sta_netif));
    }
    configure_ap_dns_from_upstream();

    if (s_ap_netif != NULL && !s_napt_enabled) {
        const esp_err_t err = esp_netif_napt_enable(s_ap_netif);
        if (err == ESP_OK) {
            s_napt_enabled = true;
            ESP_LOGI(TAG, "NAT enabled for bridge clients");
        } else {
            ESP_LOGE(TAG, "Could not enable NAT: %s", esp_err_to_name(err));
        }
    }
}

static void start_bridge_access_point(void)
{
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "RainMaker Wi-Fi station interface is missing");
        abort();
    }

    /* SoftAP provisioning creates this interface before the bridge starts. */
    s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "Could not create the bridge access point interface");
        abort();
    }

    wifi_config_t ap_config = {
        .ap = {
            .ssid = BRIDGE_SSID,
            .ssid_len = sizeof(BRIDGE_SSID) - 1,
            .channel = 1,
            .password = BRIDGE_PASSWORD,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
                .capable = true,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_netif_set_default_netif(s_sta_netif));
    configure_ap_dns_from_upstream();

    const esp_err_t err = esp_netif_napt_enable(s_ap_netif);
    if (err == ESP_OK) {
        s_napt_enabled = true;
        ESP_LOGI(TAG, "Access point ready: %s; NAT enabled", BRIDGE_SSID);
    } else {
        ESP_LOGE(TAG, "Could not enable NAT: %s", esp_err_to_name(err));
    }
}

static esp_err_t light_write_callback(const esp_rmaker_device_t *device,
                                     const esp_rmaker_param_write_req_t write_req[],
                                     uint8_t count, void *priv_data,
                                     esp_rmaker_write_ctx_t *ctx)
{
    (void)priv_data;
    if (ctx != NULL) {
        ESP_LOGI(TAG, "Light update from %s", esp_rmaker_device_cb_src_to_str(ctx->src));
    }

    for (int i = 0; i < count; i++) {
        const esp_rmaker_param_t *param = write_req[i].param;
        const esp_rmaker_param_val_t value = write_req[i].val;
        const char *name = esp_rmaker_param_get_name(param);

        if (strcmp(name, ESP_RMAKER_DEF_POWER_NAME) == 0) {
            ESP_LOGI(TAG, "RainMaker Power=%d", value.val.b);
            sunset_lamp_ble_set_power(value.val.b);
        } else if (strcmp(name, ESP_RMAKER_DEF_BRIGHTNESS_NAME) == 0) {
            ESP_LOGI(TAG, "RainMaker Brightness=%d", value.val.i);
            sunset_lamp_ble_set_brightness(value.val.i);
        } else if (strcmp(name, ESP_RMAKER_DEF_HUE_NAME) == 0) {
            ESP_LOGI(TAG, "RainMaker Hue=%d", value.val.i);
            sunset_lamp_ble_set_hue(value.val.i);
        } else if (strcmp(name, ESP_RMAKER_DEF_SATURATION_NAME) == 0) {
            ESP_LOGI(TAG, "RainMaker Saturation=%d", value.val.i);
            sunset_lamp_ble_set_saturation(value.val.i);
        }

        esp_rmaker_param_update(param, value);
    }

    ESP_LOGI(TAG, "Applied %u light parameter update(s) for %s", count,
             esp_rmaker_device_get_name(device));
    return ESP_OK;
}

static void initialize_rainmaker(void)
{
    const esp_rmaker_config_t config = {
        .enable_time_sync = false,
    };
    esp_rmaker_node_t *node = esp_rmaker_node_init(&config,
                                                  "Sunset Lamp Bridge",
                                                  "Bridge");
    if (node == NULL) {
        ESP_LOGE(TAG, "Could not initialize the RainMaker node");
        abort();
    }

    s_lamp_device = esp_rmaker_lightbulb_device_create("Sunset Lamp", NULL,
                                                       DEFAULT_LAMP_POWER);
    if (s_lamp_device == NULL) {
        ESP_LOGE(TAG, "Could not create the Sunset Lamp device");
        abort();
    }

    esp_rmaker_device_add_bulk_cb(s_lamp_device, light_write_callback, NULL);
    esp_rmaker_device_add_param(s_lamp_device,
                                esp_rmaker_brightness_param_create(
                                    ESP_RMAKER_DEF_BRIGHTNESS_NAME,
                                    DEFAULT_LAMP_BRIGHTNESS));
    esp_rmaker_device_add_param(s_lamp_device,
                                esp_rmaker_hue_param_create(
                                    ESP_RMAKER_DEF_HUE_NAME,
                                    DEFAULT_LAMP_HUE));
    esp_rmaker_device_add_param(s_lamp_device,
                                esp_rmaker_saturation_param_create(
                                    ESP_RMAKER_DEF_SATURATION_NAME,
                                    DEFAULT_LAMP_SATURATION));
    esp_rmaker_node_add_device(node, s_lamp_device);

    s_power_param = esp_rmaker_device_get_param_by_type(s_lamp_device,
                                                        ESP_RMAKER_PARAM_POWER);
    if (s_power_param == NULL) {
        ESP_LOGE(TAG, "RainMaker light has no power parameter");
        abort();
    }

    ESP_ERROR_CHECK(esp_rmaker_ota_enable_default());
    ESP_ERROR_CHECK(esp_rmaker_start());
}

static void reset_rainmaker_onboarding_if_boot_button_held(void)
{
    gpio_config_t boot_button = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_button));
    vTaskDelay(pdMS_TO_TICKS(100));

    if (gpio_get_level(GPIO_NUM_0) != 0) {
        return;
    }

    ESP_LOGW(TAG, "BOOT held: resetting Wi-Fi credentials and RainMaker app association for setup");
    ESP_ERROR_CHECK(esp_wifi_restore());

    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition("nvs", "user_mapping", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, "user_id");
        if (err == ESP_OK) {
            ESP_ERROR_CHECK(nvs_commit(handle));
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(err);
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_ERROR_CHECK(err);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_network_init();
    reset_rainmaker_onboarding_if_boot_button_held();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               ip_event_handler, NULL));
    initialize_rainmaker();

    ESP_ERROR_CHECK(app_network_set_custom_mfg_data(MFG_DATA_DEVICE_TYPE_LIGHT,
                                                     MFG_DATA_DEVICE_SUBTYPE_LIGHT));
    ESP_LOGI(TAG, "Provision RainMaker from its QR code, then the bridge will start");
    ESP_ERROR_CHECK(app_network_start(POP_TYPE_RANDOM));
    ESP_ERROR_CHECK(app_network_wait_for_network_prov_ended());

    start_bridge_access_point();
    ESP_ERROR_CHECK(sunset_lamp_ble_start(s_power_param));
}
