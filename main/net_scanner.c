/**
 * net_scanner.c
 *
 * Subnet scanner: pings every host in the local /24 and resolves hostnames.
 *
 * One ping session is created per host (count=1, 150 ms timeout).
 * On success a reverse-DNS lookup is performed via gethostbyaddr().
 * The scanner loops indefinitely, sleeping RESCAN_INTERVAL_S between sweeps.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "net_scanner.h"

static const char *TAG = "net_scan";

#define PING_TIMEOUT_MS     150
#define PING_TTL            64
#define RESCAN_INTERVAL_S   60
#define PROGRESS_EVERY      8    /* notify progress every N probes */

/* ------------------------------------------------------------------ */
/*  Callbacks (set once at start)                                      */
/* ------------------------------------------------------------------ */
static net_scan_start_cb_t    s_on_start;
static net_scan_host_cb_t     s_on_host;
static net_scan_progress_cb_t s_on_progress;
static net_scan_done_cb_t     s_on_done;
static void                  *s_ctx;

/* ------------------------------------------------------------------ */
/*  Per-ping synchronisation                                           */
/* ------------------------------------------------------------------ */
static SemaphoreHandle_t s_ping_sem;
static volatile bool     s_ping_ok;

static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    uint32_t received = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    s_ping_ok = (received > 0);
    xSemaphoreGive(s_ping_sem);
}

/* Returns true if the host at ip_be (network byte order) responds. */
static bool ping_host(uint32_t ip_be)
{
    ip_addr_t target;
    target.type         = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = ip_be;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr   = target;
    cfg.count         = 1;
    cfg.timeout_ms    = PING_TIMEOUT_MS;
    cfg.ttl           = PING_TTL;

    esp_ping_callbacks_t cbs = {
        .on_ping_end = ping_end_cb,
        .cb_args     = NULL,
    };

    esp_ping_handle_t hdl;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
        return false;
    }

    s_ping_ok = false;
    esp_ping_start(hdl);

    /* Wait for on_ping_end; use generous timeout as safety net */
    xSemaphoreTake(s_ping_sem, pdMS_TO_TICKS(PING_TIMEOUT_MS + 200));

    esp_ping_delete_session(hdl);
    return s_ping_ok;
}

/* Resolves hostname for ip_be (network byte order).
 * Writes the hostname into out, or the dotted-IP string if lookup fails. */
static void resolve_hostname(uint32_t ip_be, const char *ip_str,
                             char *out, size_t out_len)
{
    struct in_addr addr = { .s_addr = ip_be };
    struct hostent *he  = gethostbyaddr(&addr, sizeof(addr), AF_INET);
    if (he && he->h_name && he->h_name[0] != '\0') {
        strlcpy(out, he->h_name, out_len);
    } else {
        strlcpy(out, ip_str, out_len);
    }
}

/* ------------------------------------------------------------------ */
/*  Scanner task                                                       */
/* ------------------------------------------------------------------ */
static void scanner_task(void *arg)
{
    /* Wait until Ethernet has a DHCP lease */
    esp_netif_t       *netif   = NULL;
    esp_netif_ip_info_t ip_info = {0};

    ESP_LOGI(TAG, "Waiting for DHCP lease...");
    while (!netif || ip_info.ip.addr == 0) {
        netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        if (netif) {
            esp_netif_get_ip_info(netif, &ip_info);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while (1) {
        /* Re-read IP in case it changed */
        esp_netif_get_ip_info(netif, &ip_info);
        uint32_t own_h   = ntohl(ip_info.ip.addr);
        uint32_t base_h  = own_h & 0xFFFFFF00u;   /* always scan the /24 */

        ESP_LOGI(TAG, "Scan start — subnet %lu.%lu.%lu.0/24",
                 (base_h >> 24) & 0xFF,
                 (base_h >> 16) & 0xFF,
                 (base_h >>  8) & 0xFF);

        if (s_on_start) {
            s_on_start(s_ctx);
        }

        int found = 0;
        for (int i = 1; i <= 254; i++) {
            uint32_t target_h  = base_h | (uint32_t)i;
            uint32_t target_be = htonl(target_h);

            /* Format dotted-decimal string */
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), "%lu.%lu.%lu.%lu",
                     (target_h >> 24) & 0xFF,
                     (target_h >> 16) & 0xFF,
                     (target_h >>  8) & 0xFF,
                     target_h         & 0xFF);

            /* Skip our own IP */
            if (target_h == own_h) {
                if (s_on_progress) {
                    s_on_progress(i, 254, s_ctx);
                }
                continue;
            }

            bool up = ping_host(target_be);

            if (up) {
                char hostname[64];
                resolve_hostname(target_be, ip_str, hostname, sizeof(hostname));
                found++;
                ESP_LOGI(TAG, "  %s  %s", ip_str, hostname);
                if (s_on_host) {
                    s_on_host(ip_str, hostname, s_ctx);
                }
            }

            if (s_on_progress && (i % PROGRESS_EVERY == 0 || i == 254)) {
                s_on_progress(i, 254, s_ctx);
            }
        }

        ESP_LOGI(TAG, "Scan complete: %d host(s) found. Next scan in %d s.",
                 found, RESCAN_INTERVAL_S);

        if (s_on_done) {
            s_on_done(found, s_ctx);
        }

        vTaskDelay(pdMS_TO_TICKS(RESCAN_INTERVAL_S * 1000));
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
esp_err_t net_scanner_start(net_scan_start_cb_t    on_start,
                            net_scan_host_cb_t     on_host,
                            net_scan_progress_cb_t on_progress,
                            net_scan_done_cb_t     on_done,
                            void                  *ctx)
{
    s_on_start    = on_start;
    s_on_host     = on_host;
    s_on_progress = on_progress;
    s_on_done     = on_done;
    s_ctx         = ctx;

    s_ping_sem = xSemaphoreCreateBinary();
    if (!s_ping_sem) {
        ESP_LOGE(TAG, "Failed to create ping semaphore");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(scanner_task, "net_scan",
                                 6144, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scanner task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Scanner task started");
    return ESP_OK;
}
