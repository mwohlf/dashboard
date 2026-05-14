/**
 * ui_dashboard.c
 *
 * Scrollable network host list for the Waveshare 10.1" display.
 * Canvas: 1280×800 landscape (physically 800×1280, rotated 90° in sw).
 *
 * Layout
 * ──────────────────────────────────────────────────────────────────────
 *  ┌─ Header (56px) ────────────────────────────────────────────────┐
 *  │  Network Scanner          Scanning 42 / 254 …         12:34   │
 *  ├────────────────────────────────────────────────────────────────┤
 *  │  ● 192.168.1.1       router.local                             │
 *  │  ● 192.168.1.5       michael-laptop.local                     │
 *  │  ● 192.168.1.10      192.168.1.10                             │
 *  │  …                                                            │
 *  └────────────────────────────────────────────────────────────────┘
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
#define C_BG        lv_color_hex(0x0D1117)   /* near-black            */
#define C_HEADER    lv_color_hex(0x161B22)   /* slightly lighter      */
#define C_ROW_EVEN  lv_color_hex(0x0D1117)   /* same as background    */
#define C_ROW_ODD   lv_color_hex(0x131920)   /* subtle alternation    */
#define C_SEP       lv_color_hex(0x21262D)   /* separator line        */
#define C_HOST_DOT  lv_color_hex(0x2EA043)   /* green "online" dot    */
#define C_IP        lv_color_hex(0x58A6FF)   /* blue IP address       */
#define C_HOSTNAME  lv_color_hex(0xC9D1D9)   /* light grey hostname   */
#define C_DIM       lv_color_hex(0x6E7681)   /* dimmed status text    */
#define C_TITLE     lv_color_hex(0xF0F6FC)   /* bright white title    */

/* ------------------------------------------------------------------ */
/*  Layout constants                                                   */
/* ------------------------------------------------------------------ */
#define HEADER_H     56
#define ROW_H        44
#define DOT_SIZE     10
#define PAD_LEFT     20
#define IP_COL_W    160   /* fixed pixel width for IP column           */
#define HOST_COL_X  (PAD_LEFT + DOT_SIZE + 12 + IP_COL_W + 16)

/* ------------------------------------------------------------------ */
/*  Static widgets                                                     */
/* ------------------------------------------------------------------ */
static lv_obj_t *s_screen      = NULL;
static lv_obj_t *s_status_lbl  = NULL;
static lv_obj_t *s_clock_lbl   = NULL;
static lv_obj_t *s_list        = NULL;   /* scrollable host container */
static int       s_row_count   = 0;

/* ------------------------------------------------------------------ */
/*  Internal: add one row (called with LVGL lock held)                 */
/* ------------------------------------------------------------------ */
static void add_row_locked(const char *ip, const char *hostname)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(row,
        (s_row_count % 2 == 0) ? C_ROW_EVEN : C_ROW_ODD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Bottom separator line */
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);

    /* Green dot */
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, C_HOST_DOT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, PAD_LEFT, 0);

    /* IP address label */
    lv_obj_t *ip_lbl = lv_label_create(row);
    lv_obj_set_style_text_font(ip_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ip_lbl, C_IP, LV_PART_MAIN);
    lv_obj_set_width(ip_lbl, IP_COL_W);
    lv_label_set_long_mode(ip_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(ip_lbl, ip);
    lv_obj_align(ip_lbl, LV_ALIGN_LEFT_MID, PAD_LEFT + DOT_SIZE + 12, 0);

    /* Hostname label */
    int hostname_w = LVGL_H_RES - HOST_COL_X - PAD_LEFT;
    lv_obj_t *host_lbl = lv_label_create(row);
    lv_obj_set_style_text_font(host_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(host_lbl, C_HOSTNAME, LV_PART_MAIN);
    lv_obj_set_width(host_lbl, hostname_w);
    lv_label_set_long_mode(host_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(host_lbl, hostname);
    lv_obj_align(host_lbl, LV_ALIGN_LEFT_MID, HOST_COL_X, 0);

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

    /* Title */
    lv_obj_t *title = lv_label_create(header);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, C_TITLE, LV_PART_MAIN);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Network Scanner");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, PAD_LEFT, 0);

    /* Status label (centre) */
    s_status_lbl = lv_label_create(header);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_lbl, C_DIM, LV_PART_MAIN);
    lv_label_set_text(s_status_lbl, "Waiting for link…");
    lv_obj_align(s_status_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Clock (right) */
    s_clock_lbl = lv_label_create(header);
    lv_obj_set_style_text_font(s_clock_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_clock_lbl, C_TITLE, LV_PART_MAIN);
    lv_label_set_text(s_clock_lbl, "--:--");
    lv_obj_align(s_clock_lbl, LV_ALIGN_RIGHT_MID, -PAD_LEFT, 0);

    /* Column header row */
    lv_obj_t *col_hdr = lv_obj_create(s_screen);
    lv_obj_set_size(col_hdr, LV_PCT(100), 30);
    lv_obj_set_pos(col_hdr, 0, HEADER_H);
    lv_obj_set_style_bg_color(col_hdr, C_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(col_hdr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(col_hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(col_hdr, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col_hdr, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(col_hdr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(col_hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ch_ip = lv_label_create(col_hdr);
    lv_obj_set_style_text_font(ch_ip, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ch_ip, C_DIM, LV_PART_MAIN);
    lv_label_set_text(ch_ip, "IP ADDRESS");
    lv_obj_align(ch_ip, LV_ALIGN_LEFT_MID, PAD_LEFT + DOT_SIZE + 12, 0);

    lv_obj_t *ch_host = lv_label_create(col_hdr);
    lv_obj_set_style_text_font(ch_host, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ch_host, C_DIM, LV_PART_MAIN);
    lv_label_set_text(ch_host, "HOSTNAME");
    lv_obj_align(ch_host, LV_ALIGN_LEFT_MID, HOST_COL_X, 0);

    /* Scrollable host list */
    int list_top = HEADER_H + 30;
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
    if (!lvgl_port_lock(100)) return;

    lv_obj_clean(s_list);
    s_row_count = 0;

    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/*  Public: add host                                                   */
/* ------------------------------------------------------------------ */
void ui_net_list_add_host(const char *ip, const char *hostname)
{
    if (!s_list || !ip || !hostname) return;
    if (!lvgl_port_lock(100)) return;

    add_row_locked(ip, hostname);

    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ */
/*  Public: set status text                                            */
/* ------------------------------------------------------------------ */
void ui_net_list_set_status(const char *text)
{
    if (!s_status_lbl || !text) return;
    if (!lvgl_port_lock(100)) return;

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
