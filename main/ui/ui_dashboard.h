#pragma once

#include "lvgl.h"

/**
 * @brief Create the network host list UI on the active LVGL display.
 *        Call once, inside lvgl_port_lock() / lvgl_port_unlock().
 */
void ui_net_list_create(void);

/**
 * @brief Remove all host rows from the list.
 *        Thread-safe — acquires the LVGL lock internally.
 */
void ui_net_list_clear(void);

/**
 * @brief Append a host row (green dot + host + ping).
 *        Thread-safe — acquires the LVGL lock internally.
 */
void ui_net_list_add_host(const char *ip, const char *hostname, uint32_t rtt_ms);

/**
 * @brief Update the header status text (e.g. "Scanning 42 / 254").
 *        Thread-safe — acquires the LVGL lock internally.
 */
void ui_net_list_set_status(const char *text);

/**
 * @brief Periodic tick: update the clock label.
 *        Call once per second. Thread-safe.
 */
void ui_net_list_tick_1s(void);
