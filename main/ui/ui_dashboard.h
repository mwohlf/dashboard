#pragma once

#include "lvgl.h"
#include "ha_client.h"

/**
 * @brief Create the tabbed dashboard UI (landscape 1280x800).
 *        Call once, inside lvgl_port_lock() / lvgl_port_unlock().
 *
 *        Tabs:  Network | Lights | Temps | Doors
 */
void ui_dashboard_create(void);

/* --- Network tab (called from scanner callbacks) --- */

/** Remove all host rows.  Thread-safe. */
void ui_dashboard_net_clear(void);

/** Append a host row.  Thread-safe. */
void ui_dashboard_net_add_host(const char *ip, const char *hostname, uint32_t rtt_ms);

/** Update the network status text (e.g. "Scanning 42 / 254").  Thread-safe. */
void ui_dashboard_net_set_status(const char *text);

/* --- HA entity updates (called from ha_client callback) --- */

/** Process an entity state update — routes to the correct tab.  Thread-safe. */
void ui_dashboard_ha_update(const ha_entity_t *entity);

/** Update the HA connection status indicator.  Thread-safe. */
void ui_dashboard_ha_conn_update(ha_conn_status_t status);

/* --- Periodic --- */

/** 1-second tick: update clock.  Thread-safe. */
void ui_dashboard_tick_1s(void);
