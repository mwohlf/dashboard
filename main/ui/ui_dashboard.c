/**
 * ui_dashboard.c
 *
 * Scrollable network host list — portrait 800×1280.
 *
 * Layout
 * ──────────────────────────────────────────────────────────────────────
 *  ┌─ Header (80px) ───────────────────────────────────────────────┐
 *  │  Network Scanner                               12:34          │
 *  │  Scanning 42 / 254 …                                          │
 *  ├───────────────────────────────────────────────────────────────┤
 *  │  HOST                                              PING       │
 *  ├───────────────────────────────────────────────────────────────┤
 *  │  ● router.local (192.168.1.1)                      2 ms       │
 *  │  ● 192.168.1.5                                     1 ms       │
 *  │  …  (scrollable)                                              │
 *  └───────────────────────────────────────────────────────────────┘
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "config.h"
#include "ui_dashboard.h"

static const char *TAG = "ui_net";

/* ------------------------------------------------------------------ */
/*  Palette                                                            */
/* ------------------------------------------------------------------ */
#define C_BG        lv_color_hex(0x0D1117)
#define C_HEADER    lv_color_hex(0x161B22)
#define C_ROW_EVEN  lv_color_hex(0x0D1117)
#define C_ROW_ODD   lv_color_hex(0x131920)
#define C_SEP       lv_color_hex(0x21262D)
#define C_HOST_DOT  lv_color_hex(0x2EA043)
#define C_IP        lv_color_hex(0x58A6FF)
#define C_HOSTNAME  lv_color_hex(0xC9D1D9)
#define C_DIM       lv_color_hex(0x6E7681)
#define C_TITLE     lv_color_hex(0xF0F6FC)

/* ------------------------------------------------------------------ */
/*  Layout                                                             */
/* ------------------------------------------------------------------ */
#define HEADER_H     80
#define COL_HDR_H    36
#define ROW_H        52
#define DOT_SIZE     12
#define PAD_H        20    /* horizontal padding */
#define PING_COL_W   80    /* right-aligned ping column */
#define HOST_COL_X   (PAD_H + DOT_SIZE + 14)
#define HOST_COL_W   (LVGL_H_RES - HOST_COL_X - PING_COL_W - PAD_H)

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */
static lv_obj_t *s_screen     = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_clock_lbl  = NULL;
static lv_obj_t *s_list       = NULL;
static int       s_row_count  = 0;

/* ------------------------------------------------------------------ */
/*  Internal: add one row (LVGL lock must be held by caller)           */
/* ------------------------------------------------------------------ */
static void add_row_locked(const char *ip, const char *hostname, uint32_t rtt_ms)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(row,
        (s_row_count % 2 == 0) ? C_ROW_EVEN : C_ROW_ODD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Green dot */
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, C_HOST_DOT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, PAD_H, 0);

    /* Host label — prefer hostname; if it equals IP, just show IP */
    char display_name[96];
    if (hostname[0] != '\0' && strcmp(hostname, ip) != 0) {
        snprintf(display_name, sizeof(display_name), "%s (%s)", hostname, ip);
    } else {
        snprintf(display_name, sizeof(display_name), "%s", ip);
    }

    lv_obj_t *host_lbl = lv_label_create(row);
    lv_obj_set_style_text_font(host_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(host_lbl, C_HOSTNAME, LV_PART_MAIN);
    lv_obj_set_width(host_lbl, HOST_COL_W);
    lv_label_set_long_mode(host_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(host_lbl, display_name);
    lv_obj_align(host_lbl, LV_ALIGN_LEFT_MID, HOST_COL_X, 0);

    /* Ping label (right-aligned) */
    char ping_str[16];
    snprintf(ping_str, sizeof(ping_str), "%lu ms", (unsigned long)rtt_ms);

    lv_obj_t *ping_lbl = lv_label_create(row);
    lv_obj_set_style_text_font(ping_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ping_lbl, C_DIM, LV_PART_MAIN);
    lv_label_set_text(ping_lbl, ping_str);
    lv_obj_align(ping_lbl, LV_ALIGN_RIGHT_MID, -PAD_H, 0);

    s_row_count++;
}

/* ------------------------------------------------------------------ */
/*  Public: create                                                     */
/* ------------------------------------------------------------------ */
void ui_net_list_create(void)
{
    s_row_count = 0;

    /* Screen */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(s_screen);

    /* Header */
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_set_size(header, LV_PCT(100), HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(header, C_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);

    /* Title */
    lv_obj_t *title = lv_label_create(header);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, C_TITLE, LV_PART_MAIN);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Network Scanner");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PAD_H, 14);

    /* Clock (top-right) */
    s_clock_lbl = lv_label_create(header);
    lv_obj_set_style_text_font(s_clock_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_clock_lbl, C_TITLE, LV_PART_MAIN);
    lv_label_set_text(s_clock_lbl, "--:--");
    lv_obj_align(s_clock_lbl, LV_ALIGN_TOP_RIGHT, -PAD_H, 14);

    /* Status (second line) */
    s_status_lbl = lv_label_create(header);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_lbl, C_DIM, LV_PART_MAIN);
    lv_label_set_text(s_status_lbl, "Waiting for link…");
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_LEFT, PAD_H, -14);

    /* Column header row */
    lv_obj_t *col_hdr = lv_obj_create(s_screen);
    lv_obj_set_size(col_hdr, LV_PCT(100), COL_HDR_H);
    lv_obj_set_pos(col_hdr, 0, HEADER_H);
    lv_obj_set_style_bg_color(col_hdr, C_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(col_hdr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(col_hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(col_hdr, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col_hdr, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(col_hdr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(col_hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ch_host = lv_label_create(col_hdr);
    lv_obj_set_style_text_font(ch_host, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ch_host, C_DIM, LV_PART_MAIN);
    lv_label_set_text(ch_host, "HOST");
    lv_obj_align(ch_host, LV_ALIGN_LEFT_MID, HOST_COL_X, 0);

    lv_obj_t *ch_ping = lv_label_create(col_hdr);
    lv_obj_set_style_text_font(ch_ping, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ch_ping, C_DIM, LV_PART_MAIN);
    lv_label_set_text(ch_ping, "PING");
    lv_obj_align(ch_ping, LV_ALIGN_RIGHT_MID, -PAD_H, 0);

    /* Scrollable host list */
    int list_top = HEADER_H + COL_HDR_H;
    s_list = lv_obj_create(s_screen);
    lv_obj_set_pos(s_list, 0, list_top);
    lv_obj_set_size(s_list, LV_PCT(100), LVGL_V_RES - list_top);
    lv_obj_set_style_bg_color(s_list, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 0, LV_PART_MAIN);
    lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    ESP_LOGI(TAG, "Net list UI created (%dx%d)", LVGL_H_RES, LVGL_V_RES);
}

/* ------------------------------------------------------------------ */
/*  Public: clear                                                      */
/* ------------------------------------------------------------------ */
void ui_net_list_clear(void)
{
    if (!s_list) return;
    if (!lvgl_port_lock(0)) return;
    lv_obj_clean(s_list);
    s_row_count = 0;
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/*  Public: add host                                                   */
/* ------------------------------------------------------------------ */
void ui_net_list_add_host(const char *ip, const char *hostname, uint32_t rtt_ms)
{
    if (!s_list || !ip || !hostname) return;
    if (!lvgl_port_lock(0)) return;  /* 0 = wait indefinitely */
    add_row_locked(ip, hostname, rtt_ms);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/*  Public: set status text                                            */
/* ------------------------------------------------------------------ */
void ui_net_list_set_status(const char *text)
{
    if (!s_status_lbl || !text) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_lbl, text);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/*  Public: clock tick                                                 */
/* ------------------------------------------------------------------ */
void ui_net_list_tick_1s(void)
{
    if (!s_clock_lbl) return;

    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &ti);

    if (!lvgl_port_lock(50)) return;
    lv_label_set_text(s_clock_lbl, buf);
    lvgl_port_unlock();
}
