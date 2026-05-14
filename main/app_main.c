/**
 * app_main.c
 *
 * Network Scanner Dashboard for Waveshare ESP32-P4-Module-DEV-KIT-C
 *
 * Boot sequence
 * ─────────────
 *  1. NVS flash init
 *  2. Ethernet (RMII, internal EMAC) — waits for DHCP lease
 *  3. SNTP time sync
 *  4. Display init (I2C → backlight → MIPI-DSI JD9365 → GT911 touch → LVGL)
 *  5. Create network list UI
 *  6. Start subnet scanner
 *  7. 1-second timer for clock updates
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "config.h"
#include "display.h"
#include "net_scanner.h"
#include "ui_dashboard.h"

static const char *TAG = "app_main";

/* ------------------------------------------------------------------ */
/*  Ethernet                                                            */
/* ------------------------------------------------------------------ */
#define ETH_CONNECTED_BIT BIT0
#define ETH_FAIL_BIT      BIT1

static EventGroupHandle_t s_eth_evt_grp;

static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == ETH_EVENT) {
        switch (id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet link up — waiting for DHCP...");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link down");
            xEventGroupSetBits(s_eth_evt_grp, ETH_FAIL_BIT);
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet stopped");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_eth_evt_grp, ETH_CONNECTED_BIT);
    }
}

static esp_err_t eth_init(void)
{
    s_eth_evt_grp = xEventGroupCreate();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                        eth_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                        eth_event_handler, NULL, NULL);

    /* MAC — internal EMAC on ESP32-P4 */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg,
                             &(eth_mac_config_t)ETH_MAC_DEFAULT_CONFIG());

    /* PHY — IP101 (change to your board's PHY if different) */
    eth_phy_config_t phy_cfg    = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr            = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num      = ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy          = esp_eth_phy_new_generic(&phy_cfg);

    esp_eth_config_t  eth_cfg   = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t  eth_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &eth_handle),
                        TAG, "eth driver install");

    ESP_RETURN_ON_ERROR(
        esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)),
        TAG, "netif attach");

    ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), TAG, "eth start");

    EventBits_t bits = xEventGroupWaitBits(s_eth_evt_grp,
                                           ETH_CONNECTED_BIT | ETH_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));
    if (bits & ETH_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Ethernet did not get an IP within 30 s — check cable and DHCP");
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/*  SNTP                                                                */
/* ------------------------------------------------------------------ */
static void sntp_sync_cb(struct timeval *tv)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
}

static void sntp_init_and_sync(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    setenv("TZ", "UTC0", 1);
    tzset();
}

/* ------------------------------------------------------------------ */
/*  Scanner callbacks → UI                                             */
/* ------------------------------------------------------------------ */
static void on_scan_start(void *ctx)
{
    ui_net_list_clear();
    ui_net_list_set_status("Scanning…");
}

static void on_scan_host(const char *ip, const char *hostname, void *ctx)
{
    ui_net_list_add_host(ip, hostname);
}

static void on_scan_progress(int probed, int total, void *ctx)
{
    char buf[40];
    snprintf(buf, sizeof(buf), "Scanning %d / %d", probed, total);
    ui_net_list_set_status(buf);
}

static void on_scan_done(int found, void *ctx)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%d host%s found", found, found == 1 ? "" : "s");
    ui_net_list_set_status(buf);
}

/* ------------------------------------------------------------------ */
/*  1-second timer                                                     */
/* ------------------------------------------------------------------ */
static void timer_1s_cb(void *arg)
{
    ui_net_list_tick_1s();
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Network Scanner — Waveshare ESP32-P4  ");
    ESP_LOGI(TAG, "========================================");

    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Ethernet (blocks until DHCP lease or timeout) */
    ESP_LOGI(TAG, "Starting Ethernet...");
    if (eth_init() != ESP_OK) {
        ESP_LOGW(TAG, "No Ethernet — scanner will wait for link in background");
    }

    /* 3. SNTP */
    sntp_init_and_sync();

    /* 4. Display */
    ESP_LOGI(TAG, "Initialising display...");
    ESP_ERROR_CHECK(display_init());

    /* 5. Create UI */
    lvgl_port_lock(0);
    ui_net_list_create();
    lvgl_port_unlock();

    /* 6. Start subnet scanner */
    ESP_ERROR_CHECK(net_scanner_start(on_scan_start,
                                      on_scan_host,
                                      on_scan_progress,
                                      on_scan_done,
                                      NULL));

    /* 7. 1-second periodic timer (clock update) */
    const esp_timer_create_args_t timer_args = {
        .callback = timer_1s_cb,
        .name     = "dash_1s",
    };
    esp_timer_handle_t timer_1s;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_1s));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_1s, 1000000ULL));

    ESP_LOGI(TAG, "Boot complete — scanner running");
}
