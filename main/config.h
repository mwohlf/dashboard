#pragma once

/* ============================================================
 * Ethernet PHY (RMII, internal EMAC)
 * Verify these pins against your board's schematic.
 * ============================================================ */
#define ETH_MDC_GPIO     GPIO_NUM_31   /* MDIO clock  */
#define ETH_MDIO_GPIO    GPIO_NUM_52   /* MDIO data   */
#define ETH_PHY_ADDR     1             /* IP101GRI PHY address */
#define ETH_PHY_RST_GPIO GPIO_NUM_51   /* IP101GRI reset pin */

/* ============================================================
 * Home Assistant connection
 * Generate a Long-Lived Access Token in HA:
 *   Profile → Security → Long-Lived Access Tokens → Create Token
 * ============================================================ */
#define HA_HOST          "192.168.1.100"   /* HA IP or hostname */
#define HA_PORT          8123
#define HA_TOKEN         "YOUR_LONG_LIVED_ACCESS_TOKEN"
#define HA_WS_URI        "ws://" HA_HOST ":" "8123" "/api/websocket"

/* ============================================================
 * Display hardware — Waveshare 10.1-DSI-TOUCH-A
 * JD9365 panel, 800×1280 portrait, 2-lane MIPI-DSI @ 1500 Mbps
 * LVGL rotated 90° in software → 1280×800 landscape
 * ============================================================ */
#define PHYS_H_RES       800
#define PHYS_V_RES       1280
#define LVGL_H_RES       800    /* portrait, no rotation — matches reference ROTATE_0 */
#define LVGL_V_RES       1280

/* DSI PHY internal LDO (must be 2500 mV) */
#define DSI_PHY_LDO_CHAN 3
#define DSI_PHY_LDO_MV   2500

/* ============================================================
 * I2C bus — shared by backlight controller + GT911 touch
 * ============================================================ */
#define I2C_SCL_PIN      GPIO_NUM_8
#define I2C_SDA_PIN      GPIO_NUM_7
#define I2C_PORT_NUM     I2C_NUM_0
#define I2C_CLK_HZ       400000

/* Display power / backlight controller (I2C addr 0x45)
 * reg 0x95: power enable (must be written before DSI panel init)
 * reg 0x96: brightness 0–255 */
#define BL_I2C_ADDR      0x45
#define BL_EN_REG        0x95
#define BL_BRIGHT_REG    0x96
#define BL_DEFAULT_LEVEL 200   /* 0–255 */

/* ============================================================
 * LVGL render buffer (lines of pixels, allocated in PSRAM)
 * ============================================================ */
#define LVGL_BUF_LINES   100

/* ============================================================
 * Dashboard layout
 * ============================================================ */
#define DASH_COLS        3      /* entity cards per row */
#define DASH_MAX_ENTITIES 32    /* max tracked entities */
