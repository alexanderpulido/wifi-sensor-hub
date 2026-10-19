/**
 * @file wifi_station.c
 * @brief Wi-Fi station implementation. See wifi_station.h for the public API.
 */
#include "wifi_station.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_station";

/** Event-group bit set while the station holds an IP address. */
#define CONNECTED_BIT BIT0

/** Event group used to signal "connected" to tasks waiting on the link. */
static EventGroupHandle_t s_events;

/** @brief Tracks link state and re-issues connect requests after drops. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        /* Wi-Fi driver is up: begin the first connection attempt. */
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        /* Link lost or join failed: tell waiters, then try again. */
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        ESP_LOGW(TAG, "Disconnected, retrying");
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        /* DHCP finished: the network is now usable, wake any waiters. */
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

esp_err_t wifi_station_start(const wifi_station_params_t *params)
{
    if (!params || !params->ssid || !params->password)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_events = xEventGroupCreate();
    if (!s_events)
    {
        return ESP_ERR_NO_MEM;
    }

    /* Start TCP/IP stack, create the default event loop that delivers Wi-Fi/IP events, 
       and create the default Wi-Fi station network interface. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* Initialise the Wi-Fi driver with default buffer/task settings. */
    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    /* Route Wi-Fi events and the "got IP" event to on_event(). */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    /* Copy credentials into the fixed-size driver fields (truncating safely). */
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, params->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, params->password, sizeof(cfg.sta.password));

    /* Minimum security accepted: reject weaker networks than WPA2 when a
     * password is given; allow open networks only when there is none. */
    cfg.sta.threshold.authmode = params->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    /* Triggers WIFI_EVENT_STA_START, which begins the connection. */
    return esp_wifi_start();
}

bool wifi_station_wait_connected(TickType_t timeout)
{
    if (!s_events)
    {
        return false;
    }

    /* Don't clear the bit on exit (pdFALSE) so every waiter is released. */
    return xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE, timeout) & CONNECTED_BIT;
}

bool wifi_station_is_connected(void)
{
    /* Check if the Wi-Fi station is currently connected by examining the event group bit. */
    return s_events && (xEventGroupGetBits(s_events) & CONNECTED_BIT);
}
