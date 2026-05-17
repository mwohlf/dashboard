/**
 * ui_dashboard.c
 *
 * Tabbed Home Assistant dashboard — landscape 1280x800.
 *
 * Layout
 * ──────────────────────────────────────────────────────────────────────
 *  ┌──────┬────────────────────────────────────────────────────────────┐
 *  │ NET  │  Header bar (status + clock)                              │
 *  │      ├───────────────────────────────────────────────────────────┤
 *  │ LGT  │                                                           │
 *  │      │  Tab content (scrollable)                                 │
 *  │ TEMP │                                                           │
 *  │      │                                                           │
 *  │ DOOR │                                                           │
 *  └──────┴───────────────────────────────────────────────────────────┘
 *
 *  Tab 0 — Network:     scrollable host list (ping scanner)
 *  Tab 1 — Lights:      all light.* entities, toggleable
 *  Tab 2 — Temps:       all sensors with device_class "temperature"
 *  Tab 3 — Doors:       all binary_sensors with device_class door/window/garage_door
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "config.h"
#include "ha_client.h"
#include "ui_dashboard.h"

static const char *TAG = "ui_dash";

/* ------------------------------------------------------------------ */
/*  Palette                                                            */
/* ------------------------------------------------------------------ */
#define C_BG          lv_color_hex(0x0D1117)
#define C_SIDEBAR     lv_color_hex(0x0D1117)
#define C_HEADER      lv_color_hex(0x161B22)
#define C_ROW_EVEN    lv_color_hex(0x0D1117)
#define C_ROW_ODD     lv_color_hex(0x131920)
#define C_SEP         lv_color_hex(0x21262D)
#define C_HOST_DOT    lv_color_hex(0x2EA043)
#define C_IP          lv_color_hex(0x58A6FF)
#define C_HOSTNAME    lv_color_hex(0xC9D1D9)
#define C_DIM         lv_color_hex(0x6E7681)
#define C_TITLE       lv_color_hex(0xF0F6FC)
#define C_TAB_BG      lv_color_hex(0x161B22)
#define C_TAB_SEL     lv_color_hex(0x1F6FEB)
#define C_TAB_TXT     lv_color_hex(0x8B949E)
#define C_TAB_TXT_SEL lv_color_hex(0xF0F6FC)
#define C_ON          lv_color_hex(0x2EA043)
#define C_OFF         lv_color_hex(0x6E7681)
#define C_WARM        lv_color_hex(0xD29922)
#define C_OPEN        lv_color_hex(0xD29922)
#define C_CLOSED      lv_color_hex(0x2EA043)
#define C_HA_OK       lv_color_hex(0x2EA043)
#define C_HA_WARN     lv_color_hex(0xD29922)
#define C_HA_ERR      lv_color_hex(0xDA3633)

/* ------------------------------------------------------------------ */
/*  Layout constants                                                   */
/* ------------------------------------------------------------------ */
#define SIDEBAR_W     100
#define HEADER_H       50
#define CONTENT_X     SIDEBAR_W
#define CONTENT_W     (LVGL_H_RES - SIDEBAR_W)
#define CONTENT_H     (LVGL_V_RES - HEADER_H)
#define ROW_H          52
#define DOT_SIZE       12
#define PAD_H          16
#define PING_COL_W     80
#define HOST_COL_X    (PAD_H + DOT_SIZE + 14)
#define HOST_COL_W    (CONTENT_W - HOST_COL_X - PING_COL_W - PAD_H)

#define TAB_COUNT      4
#define TAB_BTN_H     (LVGL_V_RES / TAB_COUNT)

/* Max entities per category */
#define MAX_LIGHTS     48
#define MAX_TEMPS      48
#define MAX_DOORS      48

/* ------------------------------------------------------------------ */
/*  Entity storage                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char     entity_id[HA_MAX_ENTITY_ID_LEN];
    char     name[HA_MAX_NAME_LEN];
    char     state[HA_MAX_STATE_LEN];
    char     unit[HA_MAX_ATTR_LEN];
    lv_obj_t *row;          /* row widget in the list */
    lv_obj_t *state_lbl;    /* state label inside the row */
    lv_obj_t *dot;          /* status dot */
    bool      used;
} entity_slot_t;

static entity_slot_t s_lights[MAX_LIGHTS];
static entity_slot_t s_temps[MAX_TEMPS];
static entity_slot_t s_doors[MAX_DOORS];

/* ------------------------------------------------------------------ */
/*  Widget state                                                       */
/* ------------------------------------------------------------------ */
static lv_obj_t *s_screen       = NULL;
static lv_obj_t *s_tab_btns[TAB_COUNT];
static lv_obj_t *s_tab_pages[TAB_COUNT];
static int        s_active_tab  = 0;

/* Header widgets */
static lv_obj_t *s_clock_lbl    = NULL;
static lv_obj_t *s_status_lbl   = NULL;
static lv_obj_t *s_ha_dot       = NULL;

/* Network tab */
static lv_obj_t *s_net_list     = NULL;
static int       s_net_row_cnt  = 0;

/* Lights / Temps / Doors lists */
static lv_obj_t *s_light_list   = NULL;
static lv_obj_t *s_temp_list    = NULL;
static lv_obj_t *s_door_list    = NULL;

/* ------------------------------------------------------------------ */
/*  Tab names and icons                                                */
/* ------------------------------------------------------------------ */
static const char *s_tab_labels[TAB_COUNT] = {
    LV_SYMBOL_WIFI,
    LV_SYMBOL_CHARGE,     /* lightbulb / electrical — closest available */
    LV_SYMBOL_TINT,       /* thermometer-ish (drop) */
    LV_SYMBOL_EYE_OPEN,   /* door/window monitoring */
};

static const char *s_tab_names[TAB_COUNT] = {
    "Net",
    "Lights",
    "Temps",
    "Doors",
};

/* Dirty flag — set by HA callbacks (any task), cleared by tick (LVGL task) */
static volatile bool s_ha_dirty = false;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
static void switch_tab(int idx);
static void tab_btn_cb(lv_event_t *e);
static void light_row_cb(lv_event_t *e);
static lv_obj_t *create_scrollable_list(lv_obj_t *parent);
static void create_net_col_header(lv_obj_t *parent);
static void sync_entity_widgets(void);

/* ------------------------------------------------------------------ */
/*  Helper: make a scrollable flex-column list                         */
/* ------------------------------------------------------------------ */
static lv_obj_t *create_scrollable_list(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(list, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 0, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    return list;
}

/* ------------------------------------------------------------------ */
/*  Helper: generic entity row                                         */
/* ------------------------------------------------------------------ */
static lv_obj_t *create_entity_row(lv_obj_t *parent, int row_idx,
                                   const char *name, const char *state_str,
                                   lv_color_t dot_color,
                                   lv_obj_t **out_dot, lv_obj_t **out_state)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(row,
        (row_idx % 2 == 0) ? C_ROW_EVEN : C_ROW_ODD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Status dot */
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, dot_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, PAD_H, 0);
    if (out_dot) *out_dot = dot;

    /* Name label */
    lv_obj_t *name_lbl = lv_label_create(row);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, C_HOSTNAME, LV_PART_MAIN);
    lv_obj_set_width(name_lbl, CONTENT_W - HOST_COL_X - PING_COL_W - PAD_H);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(name_lbl, name);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, HOST_COL_X, 0);

    /* State label (right side) */
    lv_obj_t *st_lbl = lv_label_create(row);
    lv_obj_set_style_text_font(st_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(st_lbl, C_DIM, LV_PART_MAIN);
    lv_label_set_text(st_lbl, state_str);
    lv_obj_align(st_lbl, LV_ALIGN_RIGHT_MID, -PAD_H, 0);
    if (out_state) *out_state = st_lbl;

    return row;
}

/* ------------------------------------------------------------------ */
/*  Tab switching                                                      */
/* ------------------------------------------------------------------ */
static void switch_tab(int idx)
{
    if (idx < 0 || idx >= TAB_COUNT) return;
    s_active_tab = idx;

    for (int i = 0; i < TAB_COUNT; i++) {
        bool sel = (i == idx);
        /* Button highlight */
        lv_obj_set_style_bg_color(s_tab_btns[i],
            sel ? C_TAB_SEL : C_TAB_BG, LV_PART_MAIN);

        /* Update label color */
        lv_obj_t *lbl = lv_obj_get_child(s_tab_btns[i], 0);
        if (lbl) lv_obj_set_style_text_color(lbl,
            sel ? C_TAB_TXT_SEL : C_TAB_TXT, LV_PART_MAIN);
        lv_obj_t *lbl2 = lv_obj_get_child(s_tab_btns[i], 1);
        if (lbl2) lv_obj_set_style_text_color(lbl2,
            sel ? C_TAB_TXT_SEL : C_TAB_TXT, LV_PART_MAIN);

        /* Show/hide pages */
        if (sel) {
            lv_obj_clear_flag(s_tab_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_tab_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void tab_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch_tab(idx);
}

/* ------------------------------------------------------------------ */
/*  Light toggle callback                                              */
/* ------------------------------------------------------------------ */
static void light_row_cb(lv_event_t *e)
{
    entity_slot_t *slot = (entity_slot_t *)lv_event_get_user_data(e);
    if (!slot || !slot->used) return;

    /* Extract domain from entity_id (e.g. "light" from "light.kitchen") */
    char domain[16] = "light";
    const char *dot = strchr(slot->entity_id, '.');
    if (dot) {
        size_t len = dot - slot->entity_id;
        if (len >= sizeof(domain)) len = sizeof(domain) - 1;
        memcpy(domain, slot->entity_id, len);
        domain[len] = '\0';
    }
    ha_call_service(domain, "toggle", slot->entity_id);
}

/* ------------------------------------------------------------------ */
/*  Network tab: column header                                         */
/* ------------------------------------------------------------------ */
static void create_net_col_header(lv_obj_t *parent)
{
    lv_obj_t *col_hdr = lv_obj_create(parent);
    lv_obj_set_size(col_hdr, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(col_hdr, C_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(col_hdr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(col_hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(col_hdr, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col_hdr, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(col_hdr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(col_hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(col_hdr, 0);

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
}

/* ------------------------------------------------------------------ */
/*  Entity column header (for Lights/Temps/Doors tabs)                 */
/* ------------------------------------------------------------------ */
static void create_entity_col_header(lv_obj_t *parent,
                                     const char *left, const char *right)
{
    lv_obj_t *col_hdr = lv_obj_create(parent);
    lv_obj_set_size(col_hdr, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(col_hdr, C_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(col_hdr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(col_hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(col_hdr, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col_hdr, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(col_hdr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(col_hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(col_hdr, 0);

    lv_obj_t *l = lv_label_create(col_hdr);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, C_DIM, LV_PART_MAIN);
    lv_label_set_text(l, left);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, HOST_COL_X, 0);

    lv_obj_t *r = lv_label_create(col_hdr);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(r, C_DIM, LV_PART_MAIN);
    lv_label_set_text(r, right);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -PAD_H, 0);
}

/* ------------------------------------------------------------------ */
/*  Public: create the dashboard                                       */
/* ------------------------------------------------------------------ */
void ui_dashboard_create(void)
{
    memset(s_lights, 0, sizeof(s_lights));
    memset(s_temps,  0, sizeof(s_temps));
    memset(s_doors,  0, sizeof(s_doors));
    s_net_row_cnt = 0;

    /* --- Screen --- */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(s_screen);

    /* === Left sidebar === */
    lv_obj_t *sidebar = lv_obj_create(s_screen);
    lv_obj_set_pos(sidebar, 0, 0);
    lv_obj_set_size(sidebar, SIDEBAR_W, LVGL_V_RES);
    lv_obj_set_style_bg_color(sidebar, C_SIDEBAR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sidebar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(sidebar, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_color(sidebar, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(sidebar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(sidebar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sidebar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);

    /* Tab buttons */
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *btn = lv_obj_create(sidebar);
        lv_obj_set_pos(btn, 0, i * TAB_BTN_H);
        lv_obj_set_size(btn, SIDEBAR_W, TAB_BTN_H);
        lv_obj_set_style_bg_color(btn, C_TAB_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, C_SEP, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        /* Icon */
        lv_obj_t *icon = lv_label_create(btn);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, C_TAB_TXT, LV_PART_MAIN);
        lv_label_set_text(icon, s_tab_labels[i]);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -10);

        /* Text below icon */
        lv_obj_t *txt = lv_label_create(btn);
        lv_obj_set_style_text_font(txt, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(txt, C_TAB_TXT, LV_PART_MAIN);
        lv_label_set_text(txt, s_tab_names[i]);
        lv_obj_align(txt, LV_ALIGN_CENTER, 0, 10);

        lv_obj_add_event_cb(btn, tab_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_tab_btns[i] = btn;
    }

    /* === Header bar === */
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_set_pos(header, CONTENT_X, 0);
    lv_obj_set_size(header, CONTENT_W, HEADER_H);
    lv_obj_set_style_bg_color(header, C_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, C_SEP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* HA status dot */
    s_ha_dot = lv_obj_create(header);
    lv_obj_set_size(s_ha_dot, 10, 10);
    lv_obj_set_style_radius(s_ha_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ha_dot, C_HA_ERR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ha_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ha_dot, 0, LV_PART_MAIN);
    lv_obj_align(s_ha_dot, LV_ALIGN_LEFT_MID, PAD_H, 0);

    /* Status label */
    s_status_lbl = lv_label_create(header);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_lbl, C_DIM, LV_PART_MAIN);
    lv_label_set_text(s_status_lbl, "Connecting...");
    lv_obj_align(s_status_lbl, LV_ALIGN_LEFT_MID, PAD_H + 18, 0);

    /* Clock */
    s_clock_lbl = lv_label_create(header);
    lv_obj_set_style_text_font(s_clock_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_clock_lbl, C_TITLE, LV_PART_MAIN);
    lv_label_set_text(s_clock_lbl, "--:--");
    lv_obj_align(s_clock_lbl, LV_ALIGN_RIGHT_MID, -PAD_H, 0);

    /* === Tab pages (stacked, only one visible at a time) === */
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *page = lv_obj_create(s_screen);
        lv_obj_set_pos(page, CONTENT_X, HEADER_H);
        lv_obj_set_size(page, CONTENT_W, CONTENT_H);
        lv_obj_set_style_bg_color(page, C_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(page, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(page, 0, LV_PART_MAIN);
        lv_obj_set_layout(page, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
        s_tab_pages[i] = page;
    }

    /* --- Tab 0: Network --- */
    create_net_col_header(s_tab_pages[0]);
    s_net_list = create_scrollable_list(s_tab_pages[0]);
    lv_obj_set_flex_grow(s_net_list, 1);

    /* --- Tab 1: Lights --- */
    create_entity_col_header(s_tab_pages[1], "LIGHT", "STATE");
    s_light_list = create_scrollable_list(s_tab_pages[1]);
    lv_obj_set_flex_grow(s_light_list, 1);

    /* --- Tab 2: Temperatures --- */
    create_entity_col_header(s_tab_pages[2], "SENSOR", "VALUE");
    s_temp_list = create_scrollable_list(s_tab_pages[2]);
    lv_obj_set_flex_grow(s_temp_list, 1);

    /* --- Tab 3: Doors / Windows --- */
    create_entity_col_header(s_tab_pages[3], "SENSOR", "STATE");
    s_door_list = create_scrollable_list(s_tab_pages[3]);
    lv_obj_set_flex_grow(s_door_list, 1);

    /* Show first tab */
    switch_tab(0);

    ESP_LOGI(TAG, "Dashboard UI created (%dx%d, %d tabs)", LVGL_H_RES, LVGL_V_RES, TAB_COUNT);
}

/* ================================================================== */
/*  Network tab public API                                             */
/* ================================================================== */

void ui_dashboard_net_clear(void)
{
    if (!s_net_list) return;
    if (!lvgl_port_lock(0)) return;
    lv_obj_clean(s_net_list);
    s_net_row_cnt = 0;
    lvgl_port_unlock();
}

void ui_dashboard_net_add_host(const char *ip, const char *hostname, uint32_t rtt_ms)
{
    if (!s_net_list || !ip || !hostname) return;
    if (!lvgl_port_lock(0)) return;

    char display_name[96];
    if (hostname[0] != '\0' && strcmp(hostname, ip) != 0) {
        snprintf(display_name, sizeof(display_name), "%s (%s)", hostname, ip);
    } else {
        snprintf(display_name, sizeof(display_name), "%s", ip);
    }

    char ping_str[16];
    snprintf(ping_str, sizeof(ping_str), "%lu ms", (unsigned long)rtt_ms);

    create_entity_row(s_net_list, s_net_row_cnt,
                      display_name, ping_str, C_HOST_DOT, NULL, NULL);
    s_net_row_cnt++;

    lvgl_port_unlock();
}

void ui_dashboard_net_set_status(const char *text)
{
    if (!s_status_lbl || !text) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_status_lbl, text);
    lvgl_port_unlock();
}

/* ================================================================== */
/*  HA entity updates                                                  */
/* ================================================================== */

/* Find or allocate a slot in an entity array */
static entity_slot_t *find_or_alloc(entity_slot_t *arr, int max,
                                    const char *entity_id)
{
    entity_slot_t *free_slot = NULL;
    for (int i = 0; i < max; i++) {
        if (arr[i].used && strcmp(arr[i].entity_id, entity_id) == 0) {
            return &arr[i];
        }
        if (!arr[i].used && !free_slot) {
            free_slot = &arr[i];
        }
    }
    return free_slot;   /* NULL if full */
}

/* Count used slots */
static int count_used(entity_slot_t *arr, int max)
{
    int n = 0;
    for (int i = 0; i < max; i++) {
        if (arr[i].used) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Store entity data (called from ANY task — no LVGL calls here)      */
/* ------------------------------------------------------------------ */
static void store_light(const ha_entity_t *entity)
{
    entity_slot_t *slot = find_or_alloc(s_lights, MAX_LIGHTS, entity->entity_id);
    if (!slot) return;
    strlcpy(slot->entity_id, entity->entity_id, sizeof(slot->entity_id));
    strlcpy(slot->name,      entity->name,      sizeof(slot->name));
    strlcpy(slot->state,     entity->state,      sizeof(slot->state));
    slot->used = true;
    s_ha_dirty = true;
}

static void store_temp(const ha_entity_t *entity)
{
    entity_slot_t *slot = find_or_alloc(s_temps, MAX_TEMPS, entity->entity_id);
    if (!slot) return;
    strlcpy(slot->entity_id, entity->entity_id, sizeof(slot->entity_id));
    strlcpy(slot->name,      entity->name,      sizeof(slot->name));
    strlcpy(slot->state,     entity->state,      sizeof(slot->state));
    strlcpy(slot->unit,      entity->unit,       sizeof(slot->unit));
    slot->used = true;
    s_ha_dirty = true;
}

static void store_door(const ha_entity_t *entity)
{
    entity_slot_t *slot = find_or_alloc(s_doors, MAX_DOORS, entity->entity_id);
    if (!slot) return;
    strlcpy(slot->entity_id, entity->entity_id, sizeof(slot->entity_id));
    strlcpy(slot->name,      entity->name,      sizeof(slot->name));
    strlcpy(slot->state,     entity->state,      sizeof(slot->state));
    slot->used = true;
    s_ha_dirty = true;
}

/* ------------------------------------------------------------------ */
/*  Sync widgets — called from tick with LVGL lock held                */
/*  Creates at most SYNC_BATCH new rows per call to avoid hogging     */
/*  the timer callback.  Updates to existing rows are unlimited.       */
/* ------------------------------------------------------------------ */
#define SYNC_BATCH  10

static void sync_entity_widgets(void)
{
    int created = 0;

    /* --- Lights --- */
    for (int i = 0; i < MAX_LIGHTS; i++) {
        entity_slot_t *slot = &s_lights[i];
        if (!slot->used) continue;
        bool is_on = (strcmp(slot->state, "on") == 0);

        if (!slot->row) {
            if (created >= SYNC_BATCH) { s_ha_dirty = true; continue; }
            slot->row = create_entity_row(s_light_list, i,
                                          slot->name, is_on ? "ON" : "OFF",
                                          is_on ? C_ON : C_OFF,
                                          &slot->dot, &slot->state_lbl);
            if (!slot->row) { s_ha_dirty = true; continue; }
            lv_obj_add_flag(slot->row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(slot->row, light_row_cb, LV_EVENT_CLICKED, slot);
            created++;
        } else {
            if (slot->state_lbl) {
                lv_label_set_text(slot->state_lbl, is_on ? "ON" : "OFF");
                lv_obj_set_style_text_color(slot->state_lbl,
                    is_on ? C_ON : C_OFF, LV_PART_MAIN);
            }
            if (slot->dot) {
                lv_obj_set_style_bg_color(slot->dot,
                    is_on ? C_ON : C_OFF, LV_PART_MAIN);
            }
        }
    }

    /* --- Temps --- */
    for (int i = 0; i < MAX_TEMPS; i++) {
        entity_slot_t *slot = &s_temps[i];
        if (!slot->used) continue;

        char val_str[HA_MAX_STATE_LEN + HA_MAX_ATTR_LEN + 2];
        snprintf(val_str, sizeof(val_str), "%s %s", slot->state, slot->unit);

        if (!slot->row) {
            if (created >= SYNC_BATCH) { s_ha_dirty = true; continue; }
            slot->row = create_entity_row(s_temp_list, i,
                                          slot->name, val_str, C_WARM,
                                          &slot->dot, &slot->state_lbl);
            if (!slot->row) { s_ha_dirty = true; continue; }
            created++;
        } else {
            if (slot->state_lbl) {
                lv_label_set_text(slot->state_lbl, val_str);
            }
        }
    }

    /* --- Doors --- */
    for (int i = 0; i < MAX_DOORS; i++) {
        entity_slot_t *slot = &s_doors[i];
        if (!slot->used) continue;
        bool is_open = (strcmp(slot->state, "on") == 0);
        const char *state_str = is_open ? "OPEN" : "CLOSED";
        lv_color_t dot_col    = is_open ? C_OPEN : C_CLOSED;

        if (!slot->row) {
            if (created >= SYNC_BATCH) { s_ha_dirty = true; continue; }
            slot->row = create_entity_row(s_door_list, i,
                                          slot->name, state_str, dot_col,
                                          &slot->dot, &slot->state_lbl);
            if (!slot->row) { s_ha_dirty = true; continue; }
            created++;
        } else {
            if (slot->state_lbl) {
                lv_label_set_text(slot->state_lbl, state_str);
                lv_obj_set_style_text_color(slot->state_lbl,
                    is_open ? C_OPEN : C_CLOSED, LV_PART_MAIN);
            }
            if (slot->dot) {
                lv_obj_set_style_bg_color(slot->dot, dot_col, LV_PART_MAIN);
            }
        }
    }

    if (created > 0) {
        ESP_LOGI(TAG, "Created %d entity rows (%s)",
                 created, s_ha_dirty ? "more pending" : "done");
    }
}

void ui_dashboard_ha_update(const ha_entity_t *entity)
{
    if (!entity || !entity->valid) return;

    switch (entity->type) {
    case HA_ENTITY_LIGHT:
        store_light(entity);
        break;

    case HA_ENTITY_SWITCH:
        /* Only switches with "light" in the entity_id (e.g. switch.switchtoiletlight) */
        if (strcasestr(entity->entity_id, "light")) {
            store_light(entity);
        }
        break;

    case HA_ENTITY_SENSOR:
        /* Only temperature sensors */
        if (strcmp(entity->device_class, "temperature") == 0) {
            store_temp(entity);
        }
        break;

    case HA_ENTITY_BINARY_SENSOR:
        /* door, window, garage_door device classes */
        if (strcmp(entity->device_class, "door") == 0 ||
            strcmp(entity->device_class, "window") == 0 ||
            strcmp(entity->device_class, "garage_door") == 0) {
            store_door(entity);
        }
        break;

    default:
        break;
    }
}

/* ================================================================== */
/*  HA connection status                                               */
/* ================================================================== */

void ui_dashboard_ha_conn_update(ha_conn_status_t status)
{
    if (!s_ha_dot) return;
    if (!lvgl_port_lock(0)) return;

    switch (status) {
    case HA_CONN_AUTHENTICATED:
        lv_obj_set_style_bg_color(s_ha_dot, C_HA_OK, LV_PART_MAIN);
        lv_label_set_text(s_status_lbl, "HA connected");
        break;
    case HA_CONN_CONNECTING:
        lv_obj_set_style_bg_color(s_ha_dot, C_HA_WARN, LV_PART_MAIN);
        lv_label_set_text(s_status_lbl, "HA connecting...");
        break;
    case HA_CONN_DISCONNECTED:
        lv_obj_set_style_bg_color(s_ha_dot, C_HA_ERR, LV_PART_MAIN);
        lv_label_set_text(s_status_lbl, "HA disconnected");
        break;
    }

    lvgl_port_unlock();
}

/* ================================================================== */
/*  Clock tick                                                         */
/* ================================================================== */

void ui_dashboard_tick_1s(void)
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

    /* Sync HA entity widgets if data changed */
    if (s_ha_dirty) {
        s_ha_dirty = false;
        sync_entity_widgets();
    }

    lvgl_port_unlock();
}
