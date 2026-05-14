#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Callback fired at the start of each scan cycle (after the inter-scan sleep).
 * Use it to clear the UI list.
 */
typedef void (*net_scan_start_cb_t)(void *ctx);

/**
 * Callback fired each time a responding host is found.
 * @param ip       Dotted-decimal string, e.g. "192.168.1.10"
 * @param hostname Resolved hostname, or the IP string if DNS returned nothing
 * @param ctx      User pointer passed to net_scanner_start()
 */
typedef void (*net_scan_host_cb_t)(const char *ip, const char *hostname, void *ctx);

/**
 * Callback fired periodically during a scan with a progress update.
 * @param probed  Number of hosts probed so far
 * @param total   Total hosts to probe (254)
 */
typedef void (*net_scan_progress_cb_t)(int probed, int total, void *ctx);

/**
 * Callback fired when a full scan sweep is complete.
 * @param found  Number of responding hosts found
 */
typedef void (*net_scan_done_cb_t)(int found, void *ctx);

/**
 * @brief Start the background network scanner task.
 *
 * The task waits for a DHCP lease, then repeatedly scans all 254 hosts
 * of the local /24 subnet using ICMP ping (150 ms timeout per host).
 * For each responding host a reverse-DNS lookup is attempted.
 *
 * The scan cycle repeats every RESCAN_INTERVAL_S seconds (default 60).
 * All callbacks are invoked from the scanner task — acquire the LVGL
 * lock before touching any lv_* objects inside them.
 *
 * @param on_start     Called once at the beginning of each scan (may be NULL)
 * @param on_host      Called for every live host found       (may be NULL)
 * @param on_progress  Called every probe step                (may be NULL)
 * @param on_done      Called when the full sweep is finished (may be NULL)
 * @param ctx          Passed as-is to every callback
 */
esp_err_t net_scanner_start(net_scan_start_cb_t    on_start,
                            net_scan_host_cb_t     on_host,
                            net_scan_progress_cb_t on_progress,
                            net_scan_done_cb_t     on_done,
                            void                  *ctx);
