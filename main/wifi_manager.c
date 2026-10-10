/**
 * @brief WiFi Manager - 常开 AP (配置后台) + STA (校园网)
 */

#include "wifi_manager.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

static const char* TAG = "WIFI";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_RECONNECT_BIT  BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static wifi_state_t s_wifi_state = WIFI_STATE_DISCONNECTED;
static esp_event_handler_instance_t s_wifi_event_handle = NULL;
static esp_netif_t *s_ap_netif = NULL;

/* 延迟重连任务: 避免在事件回调中阻塞 */
static void reconnect_task(void* arg)
{
    vTaskDelay(pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "尝试重连...");
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "STA_START");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START)
    {
        esp_netif_ip_info_t info;
        if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK)
            ESP_LOGI(TAG, "AP_START IP=" IPSTR " GW=" IPSTR, IP2STR(&info.ip), IP2STR(&info.gw));
        else
            ESP_LOGW(TAG, "AP_START 但无法读取 AP IP");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "AP 客户端已连接: " MACSTR, MAC2STR(event->mac));
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "STA 已关联 AP");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGW(TAG, "STA 断开 reason=%d, 10s 后重连", d->reason);
        s_wifi_state = WIFI_STATE_DISCONNECTED;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        /* 启动独立任务延迟重连, 不阻塞事件循环 */
        xTaskCreate(reconnect_task, "wifi_reconn", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "STA 已获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_state = WIFI_STATE_CONNECTED;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return ESP_FAIL;

    esp_netif_ip_info_t ap_ip = {0};
    IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    esp_err_t dhcp_ret = esp_netif_dhcps_stop(s_ap_netif);
    if (dhcp_ret != ESP_OK) ESP_LOGW(TAG, "停止 AP DHCP 返回: %s", esp_err_to_name(dhcp_ret));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ap_ip));
    dhcp_ret = esp_netif_dhcps_start(s_ap_netif);
    if (dhcp_ret != ESP_OK) ESP_LOGW(TAG, "启动 AP DHCP 返回: %s", esp_err_to_name(dhcp_ret));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
        IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    /* AP 配置：始终开启，WPA2 保护 */
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESurfing-Config",
            .ssid_len = 15,
            .channel = 6,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .password = "esurfing2024",
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* SuperMini 降功率 */
    esp_wifi_set_max_tx_power(34);

    ESP_LOGI(TAG, "AP 已开启: ESurfing-Config (192.168.4.1), DHCP=192.168.4.2-192.168.4.20");
    return ESP_OK;
}

esp_err_t wifi_connect(const char* ssid, const char* password)
{
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password && password[0])
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    /* 空闲期间使用最大 Modem-Sleep；AP 仍保持开启，STA 按 DTIM 周期唤醒。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
    ESP_LOGI(TAG, "STA 启用 WiFi 最大省电模式");
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "STA 连接: [%s]", ssid);
    esp_wifi_connect();
    return ESP_OK;
}

wifi_state_t wifi_get_state(void) { return s_wifi_state; }

bool wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_wifi_state == WIFI_STATE_CONNECTED) return true;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_is_connected(void) { return s_wifi_state == WIFI_STATE_CONNECTED; }

int wifi_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

esp_err_t wifi_reconnect(void)
{
    s_wifi_state = WIFI_STATE_DISCONNECTED;
    return esp_wifi_connect();
}
