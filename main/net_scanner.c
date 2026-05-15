/**
 * net_scanner.c
 *
 * Subnet scanner: pings every host in the local /24 using a single raw
 * ICMP socket (no per-host esp_ping session overhead).
 *
 * The scanner loops indefinitely, sleeping RESCAN_INTERVAL_S between sweeps.
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/ip4.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "net_scanner.h"

static const char *TAG = "net_scan";

#define PING_TIMEOUT_MS     150
#define PING_TTL            64
#define RESCAN_INTERVAL_S   60
#define PROGRESS_EVERY      8    /* notify progress every N probes */
#define ICMP_ECHO_REQUEST   8
#define ICMP_ECHO_REPLY     0
#define ICMP_PAYLOAD_SIZE   32

/* ------------------------------------------------------------------ */
/*  Callbacks (set once at start)                                      */
/* ------------------------------------------------------------------ */
static net_scan_start_cb_t    s_on_start;
static net_scan_host_cb_t     s_on_host;
static net_scan_progress_cb_t s_on_progress;
static net_scan_done_cb_t     s_on_done;
static void                  *s_ctx;

/* ------------------------------------------------------------------ */
/*  ICMP echo using a single raw socket                                */
/* ------------------------------------------------------------------ */
static uint16_t s_ping_id;
static uint16_t s_ping_seq;

/* Build an ICMP echo request packet. Returns total packet size. */
static int build_echo_request(uint8_t *buf, size_t buflen)
{
    if (buflen < sizeof(struct icmp_echo_hdr) + ICMP_PAYLOAD_SIZE) {
        return -1;
    }

    struct icmp_echo_hdr *hdr = (struct icmp_echo_hdr *)buf;
    hdr->type  = ICMP_ECHO_REQUEST;
    hdr->code  = 0;
    hdr->chksum = 0;
    hdr->id    = htons(s_ping_id);
    hdr->seqno = htons(s_ping_seq);

    /* Fill payload with pattern */
    memset(buf + sizeof(struct icmp_echo_hdr), 0xAB, ICMP_PAYLOAD_SIZE);

    int pkt_len = sizeof(struct icmp_echo_hdr) + ICMP_PAYLOAD_SIZE;
    hdr->chksum = inet_chksum(buf, pkt_len);
    return pkt_len;
}

/* Send one ICMP echo and wait for reply.
 * Returns true on success; *out_rtt_ms receives the round-trip time. */
static bool ping_host(int sock, uint32_t ip_be, uint32_t *out_rtt_ms)
{
    uint8_t pkt[sizeof(struct icmp_echo_hdr) + ICMP_PAYLOAD_SIZE];
    s_ping_seq++;
    int pkt_len = build_echo_request(pkt, sizeof(pkt));
    if (pkt_len < 0) {
        return false;
    }

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = ip_be,
    };

    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

    if (sendto(sock, pkt, pkt_len, 0,
               (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        return false;
    }

    /* Wait for matching reply */
    uint8_t recv_buf[128];
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                           (struct sockaddr *)&from, &fromlen);
        if (len < 0) {
            /* Timeout or error */
            return false;
        }

        /* Parse IP header + ICMP */
        if (len < (int)(sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr))) {
            continue;
        }
        struct ip_hdr *iphdr = (struct ip_hdr *)recv_buf;
        int ip_hdr_len = IPH_HL_BYTES(iphdr);
        struct icmp_echo_hdr *reply = (struct icmp_echo_hdr *)(recv_buf + ip_hdr_len);

        if (reply->type == ICMP_ECHO_REPLY &&
            ntohs(reply->id) == s_ping_id &&
            ntohs(reply->seqno) == s_ping_seq) {
            gettimeofday(&t_end, NULL);
            uint32_t ms = (uint32_t)((t_end.tv_sec - t_start.tv_sec) * 1000 +
                                     (t_end.tv_usec - t_start.tv_usec) / 1000);
            *out_rtt_ms = ms;
            return true;
        }
        /* Not our reply — keep reading until timeout */
    }
}

/* lwIP does not implement gethostbyaddr (reverse DNS).
 * Hostname column shows the IP address; mDNS support can be added later. */
static void resolve_hostname(const char *ip_str, char *out, size_t out_len)
{
    strlcpy(out, ip_str, out_len);
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

    /* Use task start tick as unique ping ID */
    s_ping_id = (uint16_t)(xTaskGetTickCount() & 0xFFFF);
    s_ping_seq = 0;

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

        /* Create one raw ICMP socket for the entire scan */
        int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create ICMP socket (%d)", errno);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int ttl = PING_TTL;
        setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));

        struct timeval tv = {
            .tv_sec  = PING_TIMEOUT_MS / 1000,
            .tv_usec = (PING_TIMEOUT_MS % 1000) * 1000,
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

            uint32_t rtt_ms = 0;
            bool up = ping_host(sock, target_be, &rtt_ms);

            if (up) {
                char hostname[64];
                resolve_hostname(ip_str, hostname, sizeof(hostname));
                found++;
                ESP_LOGI(TAG, "  %s  %s  %lu ms", ip_str, hostname, (unsigned long)rtt_ms);
                if (s_on_host) {
                    s_on_host(ip_str, hostname, rtt_ms, s_ctx);
                }
            }

            if (s_on_progress && (i % PROGRESS_EVERY == 0 || i == 254)) {
                s_on_progress(i, 254, s_ctx);
            }
        }

        close(sock);

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

    BaseType_t ret = xTaskCreate(scanner_task, "net_scan",
                                 6144, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scanner task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Scanner task started");
    return ESP_OK;
}
