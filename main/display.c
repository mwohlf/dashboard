/**
 * display.c
 *
 * Brings up the Waveshare 10.1-DSI-TOUCH-A display on the
 * ESP32-P4-Module-DEV-KIT-C using pure Espressif components.
 *
 * Hardware path:
 *   ESP32-P4 MIPI-DSI (2-lane) → JD9365 panel IC → 800×1280 IPS LCD
 *   I2C (GPIO7/8) → backlight controller (0x45) + GT911 touch
 *
 * Key facts (from Waveshare + Harald Kreuzer research):
 *   - Panel: JD9365, espressif/esp_lcd_jd9365 component
 *   - Backlight: I2C addr 0x45, reg 0x96 (level 0–255)
 *   - Touch: GT911 at ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS
 *   - PHY LDO: channel 3, 2500 mV
 *   - 2-lane DSI @ 1500 Mbps, vsync_back_porch = 10 (Waveshare specific)
 *   - BTA workaround required: v1.3 silicon hangs on DCS writes with cmd_ack=true
 *   - Display is physically portrait-only; use LVGL sw_rotate for landscape
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_host_ll.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "config.h"
#include "display.h"

static const char *TAG = "display";

/* ------------------------------------------------------------------ */
/*  Module-private handles                                              */
/* ------------------------------------------------------------------ */
static i2c_master_bus_handle_t  s_i2c_bus   = NULL;
static i2c_master_dev_handle_t  s_bl_dev    = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus   = NULL;
static esp_lcd_panel_handle_t   s_panel     = NULL;
static esp_lcd_touch_handle_t   s_touch     = NULL;
static lv_disp_t               *s_lvgl_disp = NULL;

/* Waveshare 10.1-DSI-TOUCH-A init sequence.
 * Source: waveshareteam/Waveshare-ESP32-components, display/lcd/esp_lcd_jd9365_10_1.
 * The generic Espressif JD9365 driver's built-in default has different GIP timing and
 * register values that don't work for this specific panel variant. */
static const jd9365_lcd_init_cmd_t s_ws_init_cmds[] = {
    /* Page 0 — unlock */
    {0xE0, (uint8_t []){0x00}, 1, 0},
    {0xE1, (uint8_t []){0x93}, 1, 0},
    {0xE2, (uint8_t []){0x65}, 1, 0},
    {0xE3, (uint8_t []){0xF8}, 1, 0},
    {0x80, (uint8_t []){0x01}, 1, 0},   /* DSI 2-lane mode */
    /* Page 1 — power / VCOM / timing */
    {0xE0, (uint8_t []){0x01}, 1, 0},
    {0x00, (uint8_t []){0x00}, 1, 0},
    {0x01, (uint8_t []){0x38}, 1, 0},
    {0x03, (uint8_t []){0x10}, 1, 0},
    {0x04, (uint8_t []){0x38}, 1, 0},
    {0x0C, (uint8_t []){0x74}, 1, 0},
    {0x17, (uint8_t []){0x00}, 1, 0},
    {0x18, (uint8_t []){0xAF}, 1, 0},
    {0x19, (uint8_t []){0x00}, 1, 0},
    {0x1A, (uint8_t []){0x00}, 1, 0},
    {0x1B, (uint8_t []){0xAF}, 1, 0},
    {0x1C, (uint8_t []){0x00}, 1, 0},
    {0x35, (uint8_t []){0x26}, 1, 0},
    {0x37, (uint8_t []){0x09}, 1, 0},
    {0x38, (uint8_t []){0x04}, 1, 0},
    {0x39, (uint8_t []){0x00}, 1, 0},
    {0x3A, (uint8_t []){0x01}, 1, 0},
    {0x3C, (uint8_t []){0x78}, 1, 0},
    {0x3D, (uint8_t []){0xFF}, 1, 0},
    {0x3E, (uint8_t []){0xFF}, 1, 0},
    {0x3F, (uint8_t []){0x7F}, 1, 0},
    {0x40, (uint8_t []){0x06}, 1, 0},
    {0x41, (uint8_t []){0xA0}, 1, 0},
    {0x42, (uint8_t []){0x81}, 1, 0},
    {0x43, (uint8_t []){0x1E}, 1, 0},
    {0x44, (uint8_t []){0x0D}, 1, 0},
    {0x45, (uint8_t []){0x28}, 1, 0},
    {0x55, (uint8_t []){0x02}, 1, 0},
    {0x57, (uint8_t []){0x69}, 1, 0},
    {0x59, (uint8_t []){0x0A}, 1, 0},
    {0x5A, (uint8_t []){0x2A}, 1, 0},
    {0x5B, (uint8_t []){0x17}, 1, 0},
    /* Gamma positive */
    {0x5D, (uint8_t []){0x7F}, 1, 0},
    {0x5E, (uint8_t []){0x6A}, 1, 0},
    {0x5F, (uint8_t []){0x5B}, 1, 0},
    {0x60, (uint8_t []){0x4F}, 1, 0},
    {0x61, (uint8_t []){0x4A}, 1, 0},
    {0x62, (uint8_t []){0x3D}, 1, 0},
    {0x63, (uint8_t []){0x41}, 1, 0},
    {0x64, (uint8_t []){0x2A}, 1, 0},
    {0x65, (uint8_t []){0x44}, 1, 0},
    {0x66, (uint8_t []){0x43}, 1, 0},
    {0x67, (uint8_t []){0x44}, 1, 0},
    {0x68, (uint8_t []){0x62}, 1, 0},
    {0x69, (uint8_t []){0x52}, 1, 0},
    {0x6A, (uint8_t []){0x59}, 1, 0},
    {0x6B, (uint8_t []){0x4C}, 1, 0},
    {0x6C, (uint8_t []){0x48}, 1, 0},
    {0x6D, (uint8_t []){0x3A}, 1, 0},
    {0x6E, (uint8_t []){0x26}, 1, 0},
    {0x6F, (uint8_t []){0x00}, 1, 0},
    /* Gamma negative */
    {0x70, (uint8_t []){0x7F}, 1, 0},
    {0x71, (uint8_t []){0x6A}, 1, 0},
    {0x72, (uint8_t []){0x5B}, 1, 0},
    {0x73, (uint8_t []){0x4F}, 1, 0},
    {0x74, (uint8_t []){0x4A}, 1, 0},
    {0x75, (uint8_t []){0x3D}, 1, 0},
    {0x76, (uint8_t []){0x41}, 1, 0},
    {0x77, (uint8_t []){0x2A}, 1, 0},
    {0x78, (uint8_t []){0x44}, 1, 0},
    {0x79, (uint8_t []){0x43}, 1, 0},
    {0x7A, (uint8_t []){0x44}, 1, 0},
    {0x7B, (uint8_t []){0x62}, 1, 0},
    {0x7C, (uint8_t []){0x52}, 1, 0},
    {0x7D, (uint8_t []){0x59}, 1, 0},
    {0x7E, (uint8_t []){0x4C}, 1, 0},
    {0x7F, (uint8_t []){0x48}, 1, 0},
    {0x80, (uint8_t []){0x3A}, 1, 0},
    {0x81, (uint8_t []){0x26}, 1, 0},
    {0x82, (uint8_t []){0x00}, 1, 0},
    /* Page 2 — GIP mapping */
    {0xE0, (uint8_t []){0x02}, 1, 0},
    {0x00, (uint8_t []){0x42}, 1, 0},
    {0x01, (uint8_t []){0x42}, 1, 0},
    {0x02, (uint8_t []){0x40}, 1, 0},
    {0x03, (uint8_t []){0x40}, 1, 0},
    {0x04, (uint8_t []){0x5E}, 1, 0},
    {0x05, (uint8_t []){0x5E}, 1, 0},
    {0x06, (uint8_t []){0x5F}, 1, 0},
    {0x07, (uint8_t []){0x5F}, 1, 0},
    {0x08, (uint8_t []){0x5F}, 1, 0},
    {0x09, (uint8_t []){0x57}, 1, 0},
    {0x0A, (uint8_t []){0x57}, 1, 0},
    {0x0B, (uint8_t []){0x77}, 1, 0},
    {0x0C, (uint8_t []){0x77}, 1, 0},
    {0x0D, (uint8_t []){0x47}, 1, 0},
    {0x0E, (uint8_t []){0x47}, 1, 0},
    {0x0F, (uint8_t []){0x45}, 1, 0},
    {0x10, (uint8_t []){0x45}, 1, 0},
    {0x11, (uint8_t []){0x4B}, 1, 0},
    {0x12, (uint8_t []){0x4B}, 1, 0},
    {0x13, (uint8_t []){0x49}, 1, 0},
    {0x14, (uint8_t []){0x49}, 1, 0},
    {0x15, (uint8_t []){0x5F}, 1, 0},
    {0x16, (uint8_t []){0x41}, 1, 0},
    {0x17, (uint8_t []){0x41}, 1, 0},
    {0x18, (uint8_t []){0x40}, 1, 0},
    {0x19, (uint8_t []){0x40}, 1, 0},
    {0x1A, (uint8_t []){0x5E}, 1, 0},
    {0x1B, (uint8_t []){0x5E}, 1, 0},
    {0x1C, (uint8_t []){0x5F}, 1, 0},
    {0x1D, (uint8_t []){0x5F}, 1, 0},
    {0x1E, (uint8_t []){0x5F}, 1, 0},
    {0x1F, (uint8_t []){0x57}, 1, 0},
    {0x20, (uint8_t []){0x57}, 1, 0},
    {0x21, (uint8_t []){0x77}, 1, 0},
    {0x22, (uint8_t []){0x77}, 1, 0},
    {0x23, (uint8_t []){0x46}, 1, 0},
    {0x24, (uint8_t []){0x46}, 1, 0},
    {0x25, (uint8_t []){0x44}, 1, 0},
    {0x26, (uint8_t []){0x44}, 1, 0},
    {0x27, (uint8_t []){0x4A}, 1, 0},
    {0x28, (uint8_t []){0x4A}, 1, 0},
    {0x29, (uint8_t []){0x48}, 1, 0},
    {0x2A, (uint8_t []){0x48}, 1, 0},
    {0x2B, (uint8_t []){0x5F}, 1, 0},
    {0x2C, (uint8_t []){0x01}, 1, 0},
    {0x2D, (uint8_t []){0x01}, 1, 0},
    {0x2E, (uint8_t []){0x00}, 1, 0},
    {0x2F, (uint8_t []){0x00}, 1, 0},
    {0x30, (uint8_t []){0x1F}, 1, 0},
    {0x31, (uint8_t []){0x1F}, 1, 0},
    {0x32, (uint8_t []){0x1E}, 1, 0},
    {0x33, (uint8_t []){0x1E}, 1, 0},
    {0x34, (uint8_t []){0x1F}, 1, 0},
    {0x35, (uint8_t []){0x17}, 1, 0},
    {0x36, (uint8_t []){0x17}, 1, 0},
    {0x37, (uint8_t []){0x37}, 1, 0},
    {0x38, (uint8_t []){0x37}, 1, 0},
    {0x39, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x08}, 1, 0},
    {0x3B, (uint8_t []){0x0A}, 1, 0},
    {0x3C, (uint8_t []){0x0A}, 1, 0},
    {0x3D, (uint8_t []){0x04}, 1, 0},
    {0x3E, (uint8_t []){0x04}, 1, 0},
    {0x3F, (uint8_t []){0x06}, 1, 0},
    {0x40, (uint8_t []){0x06}, 1, 0},
    {0x41, (uint8_t []){0x1F}, 1, 0},
    {0x42, (uint8_t []){0x02}, 1, 0},
    {0x43, (uint8_t []){0x02}, 1, 0},
    {0x44, (uint8_t []){0x00}, 1, 0},
    {0x45, (uint8_t []){0x00}, 1, 0},
    {0x46, (uint8_t []){0x1F}, 1, 0},
    {0x47, (uint8_t []){0x1F}, 1, 0},
    {0x48, (uint8_t []){0x1E}, 1, 0},
    {0x49, (uint8_t []){0x1E}, 1, 0},
    {0x4A, (uint8_t []){0x1F}, 1, 0},
    {0x4B, (uint8_t []){0x17}, 1, 0},
    {0x4C, (uint8_t []){0x17}, 1, 0},
    {0x4D, (uint8_t []){0x37}, 1, 0},
    {0x4E, (uint8_t []){0x37}, 1, 0},
    {0x4F, (uint8_t []){0x09}, 1, 0},
    {0x50, (uint8_t []){0x09}, 1, 0},
    {0x51, (uint8_t []){0x0B}, 1, 0},
    {0x52, (uint8_t []){0x0B}, 1, 0},
    {0x53, (uint8_t []){0x05}, 1, 0},
    {0x54, (uint8_t []){0x05}, 1, 0},
    {0x55, (uint8_t []){0x07}, 1, 0},
    {0x56, (uint8_t []){0x07}, 1, 0},
    {0x57, (uint8_t []){0x1F}, 1, 0},
    /* GIP timing */
    {0x58, (uint8_t []){0x40}, 1, 0},
    {0x5B, (uint8_t []){0x30}, 1, 0},
    {0x5C, (uint8_t []){0x00}, 1, 0},
    {0x5D, (uint8_t []){0x34}, 1, 0},
    {0x5E, (uint8_t []){0x05}, 1, 0},
    {0x5F, (uint8_t []){0x02}, 1, 0},
    {0x63, (uint8_t []){0x00}, 1, 0},
    {0x64, (uint8_t []){0x6A}, 1, 0},
    {0x67, (uint8_t []){0x73}, 1, 0},
    {0x68, (uint8_t []){0x07}, 1, 0},
    {0x69, (uint8_t []){0x08}, 1, 0},
    {0x6A, (uint8_t []){0x6A}, 1, 0},
    {0x6B, (uint8_t []){0x08}, 1, 0},
    {0x6C, (uint8_t []){0x00}, 1, 0},
    {0x6D, (uint8_t []){0x00}, 1, 0},
    {0x6E, (uint8_t []){0x00}, 1, 0},
    {0x6F, (uint8_t []){0x88}, 1, 0},
    {0x75, (uint8_t []){0xFF}, 1, 0},
    {0x77, (uint8_t []){0xDD}, 1, 0},
    {0x78, (uint8_t []){0x2C}, 1, 0},
    {0x79, (uint8_t []){0x15}, 1, 0},
    {0x7A, (uint8_t []){0x17}, 1, 0},
    {0x7D, (uint8_t []){0x14}, 1, 0},
    {0x7E, (uint8_t []){0x82}, 1, 0},
    /* Page 4 */
    {0xE0, (uint8_t []){0x04}, 1, 0},
    {0x00, (uint8_t []){0x0E}, 1, 0},
    {0x02, (uint8_t []){0xB3}, 1, 0},
    {0x09, (uint8_t []){0x61}, 1, 0},
    {0x0E, (uint8_t []){0x48}, 1, 0},
    {0x37, (uint8_t []){0x58}, 1, 0},
    {0x2B, (uint8_t []){0x0F}, 1, 0},
    /* Back to Page 0 — final */
    {0xE0, (uint8_t []){0x00}, 1, 0},
    {0xE6, (uint8_t []){0x02}, 1, 0},
    {0xE7, (uint8_t []){0x0C}, 1, 0},
    /* Sleep out + display on */
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 20},
};

/* ------------------------------------------------------------------ */
/*  I2C bus init                                                        */
/* ------------------------------------------------------------------ */
static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source             = I2C_CLK_SRC_DEFAULT,
        .i2c_port               = I2C_PORT_NUM,
        .scl_io_num             = I2C_SCL_PIN,
        .sda_io_num             = I2C_SDA_PIN,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus),
                        TAG, "i2c_new_master_bus failed");
    ESP_LOGI(TAG, "I2C bus ready (SCL=%d SDA=%d %dHz)",
             I2C_SCL_PIN, I2C_SDA_PIN, I2C_CLK_HZ);

    /* Scan — log every responding address so we can identify the backlight IC */
    ESP_LOGI(TAG, "I2C scan:");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 10) == ESP_OK) {
            ESP_LOGI(TAG, "  device at 0x%02X", addr);
        }
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Backlight controller (I2C address 0x45)                            */
/* ------------------------------------------------------------------ */
static esp_err_t init_backlight(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BL_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_bl_dev),
                        TAG, "add backlight I2C device failed");

    /* Power-enable sequence from Waveshare BSP — register 0x95 controls the
     * display power supply.  Without this, the JD9365 panel IC has no power
     * and the screen stays blank regardless of DSI/DPI output. */
    uint8_t en1[] = {BL_EN_REG, 0x11};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_bl_dev, en1, sizeof(en1), 50), TAG, "BL en1");
    uint8_t en2[] = {BL_EN_REG, 0x17};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_bl_dev, en2, sizeof(en2), 50), TAG, "BL en2");

    /* Brightness off initially */
    uint8_t bl_off[] = {BL_BRIGHT_REG, 0x00};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_bl_dev, bl_off, sizeof(bl_off), 50), TAG, "BL off");

    /* Waveshare BSP waits 100 ms after enable + 1 s for power to stabilize
     * before creating the DSI panel.  We do the delay here so init_panel()
     * sees stable rails. */
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t bl_max[] = {BL_BRIGHT_REG, 0xFF};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_bl_dev, bl_max, sizeof(bl_max), 50), TAG, "BL max");
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Display power enabled + backlight on");
    return ESP_OK;
}

esp_err_t display_set_brightness(uint8_t val)
{
    uint8_t cmd[] = {BL_BRIGHT_REG, val};
    return i2c_master_transmit(s_bl_dev, cmd, sizeof(cmd), 50);
}

/* ------------------------------------------------------------------ */
/*  MIPI-DSI panel (JD9365) init                                       */
/* ------------------------------------------------------------------ */
static esp_err_t init_panel(void)
{
    /* 1. Power the MIPI-DSI PHY via the P4's internal LDO */
    esp_ldo_channel_handle_t phy_pwr = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr),
                        TAG, "LDO channel acquire failed");

    /* 2. Create the DSI bus (2-lane, 1500 Mbps/lane — Waveshare BSP default) */
    esp_lcd_dsi_bus_config_t bus_cfg = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus),
                        TAG, "esp_lcd_new_dsi_bus failed");

    /* 3. DBI (command mode) IO handle — used only during init */
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &dbi_io),
                        TAG, "esp_lcd_new_panel_io_dbi failed");

    /* Workaround for ESP32-P4 v1.x silicon (confirmed v1.3 affected):
     * cmd_ack=true causes the DSI host to perform a Bus Turn-Around after
     * every DCS write and wait for an ACK that never arrives on v1.x silicon.
     * All DSI timeouts in the IDF driver are 0, so the host spin-waits on
     * gen_cmd_full indefinitely → IDLE watchdog fires.
     * The jd9365 driver notes this for reads (skips LCD ID read) but DCS writes
     * during panel_init are also affected — disable cmd_ack immediately after
     * DBI IO creation.
     * Internal esp_lcd_dsi_bus_t layout: { int bus_id; mipi_dsi_hal_context_t hal; } */
    {
        typedef struct { int bus_id; mipi_dsi_hal_context_t hal; } dsi_bus_priv_t;
        dsi_bus_priv_t *priv = (dsi_bus_priv_t *)s_dsi_bus;
        mipi_dsi_host_ll_enable_cmd_ack(priv->hal.host, false);
        ESP_LOGI(TAG, "DSI cmd_ack disabled (v1.x BTA workaround)");
    }

    /* 4. DPI (video mode) config — 800×1280 @ 60 Hz, RGB565 */
    esp_lcd_dpi_panel_config_t dpi_cfg =
        JD9365_800_1280_PANEL_60HZ_DPI_CONFIG_CF(LCD_COLOR_FMT_RGB565);
    dpi_cfg.num_fbs = 2;   /* double-buffer required: lvgl_port_add_disp_dsi with
                            * avoid_tearing=true calls esp_lcd_dpi_panel_get_frame_buffer(panel, 2, ...)
                            * and registers on_color_trans_done to call lv_disp_flush_ready(). */
    dpi_cfg.video_timing.vsync_back_porch = 10; /* Waveshare-specific tweak */

    /* 5. Vendor config — Waveshare 10.1" panel-specific init sequence */
    jd9365_vendor_config_t vendor_cfg = {
        .init_cmds      = s_ws_init_cmds,
        .init_cmds_size = sizeof(s_ws_init_cmds) / sizeof(s_ws_init_cmds[0]),
        .mipi_config = {
            .dsi_bus    = s_dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = 2,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(dbi_io, &panel_cfg, &s_panel),
                        TAG, "esp_lcd_new_panel_jd9365 failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),  TAG, "panel init");
    /* Note: no esp_lcd_panel_disp_on_off — SLEEP_OUT (0x11) + DISPLAY_ON (0x29)
     * are already at the end of vendor_specific_init_default, executed during panel_init. */

    ESP_LOGI(TAG, "JD9365 panel ready (%dx%d)", PHYS_H_RES, PHYS_V_RES);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GT911 touch controller                                              */
/* ------------------------------------------------------------------ */
static esp_err_t init_touch(void)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr            = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 0,
        .lcd_cmd_bits        = 16,
        .flags.disable_control_phase = 1,
        .scl_speed_hz        = I2C_CLK_HZ,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io),
        TAG, "touch panel IO failed");

    esp_lcd_touch_config_t touch_cfg = {
        .x_max        = PHYS_H_RES,
        .y_max        = PHYS_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels       = {.reset = 0, .interrupt = 0},
        .flags        = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(tp_io, &touch_cfg, &s_touch),
        TAG, "GT911 init failed");

    ESP_LOGI(TAG, "GT911 touch ready");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  LVGL integration via esp_lvgl_port                                  */
/* ------------------------------------------------------------------ */
static esp_err_t init_lvgl(void)
{
    /* Init LVGL port (creates LVGL task + mutex) */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init failed");

    /* Register display — portrait 800×1280, no software rotation.
     *
     * avoid_tearing=true + full_refresh=true: required pairing for DPI double-buffer.
     *   - avoid_tearing: port gets two PSRAM DPI framebuffers; registers on_refresh_done
     *     ISR which gives a semaphore at vsync.
     *   - full_refresh: flush_cb waits on that semaphore (properly blocking, not spinning),
     *     then calls lv_disp_flush_ready().  Without full_refresh, the semaphore path in
     *     flush_cb is dead code → lv_disp_flush_ready() never called → LVGL spins at
     *     lv_refr.c:709 starving IDLE and the display never updates.
     * Requires num_fbs=2 in dpi_cfg so the port can retrieve both framebuffer pointers. */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = NULL,
        .panel_handle   = s_panel,
        .control_handle = NULL,
        .buffer_size    = PHYS_H_RES * LVGL_BUF_LINES,
        .double_buffer  = false,
        .hres           = PHYS_H_RES,
        .vres           = PHYS_V_RES,
        .monochrome     = false,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma     = false,
            .buff_spiram  = false,
            .sw_rotate    = false,
            .full_refresh = true,  /* required with avoid_tearing: the port's on_refresh_done ISR
                                    * gives a semaphore; flush_cb waits on it (yielding the task),
                                    * then calls lv_disp_flush_ready().  Without full_refresh the
                                    * semaphore path is never entered and lv_disp_flush_ready() is
                                    * never called → LVGL spins at lv_refr.c:709 forever. */
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags.avoid_tearing = true,
    };
    s_lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!s_lvgl_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi returned NULL");
        return ESP_FAIL;
    }

    /* Register touch input */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = s_lvgl_disp,
        .handle = s_touch,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
    if (!indev) {
        ESP_LOGW(TAG, "lvgl_port_add_touch returned NULL (touch may not work)");
    }

    ESP_LOGI(TAG, "LVGL port ready (%dx%d portrait)", PHYS_H_RES, PHYS_V_RES);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(init_i2c(),      TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(init_backlight(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(init_panel(),    TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(init_touch(),    TAG, "touch init failed");
    ESP_RETURN_ON_ERROR(init_lvgl(),     TAG, "LVGL init failed");

    /* Set final brightness (init_backlight sets 0xFF for power-up stabilisation) */
    display_set_brightness(BL_DEFAULT_LEVEL);
    return ESP_OK;
}

lv_disp_t *display_get_lvgl_disp(void)
{
    return s_lvgl_disp;
}
