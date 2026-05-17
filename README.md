# HA Dashboard

A Home Assistant touch dashboard running on the **Waveshare ESP32-P4-Module-DEV-KIT-C** with a **Waveshare 10.1" DSI-TOUCH-A** display. Built with ESP-IDF v6.0.1 and LVGL 8.4.

## Current state

The dashboard connects to a Home Assistant instance over WebSocket and presents a 4-tab interface in landscape orientation (1280x800):

| Tab | Icon | Contents | Interaction |
|-----|------|----------|-------------|
| **Net** | WiFi | Live hosts on the local /24 subnet (IP, hostname, ping RTT). Rescans every 60 s. | Read-only |
| **Lights** | Charge (bulb) | All `light.*` entities + `switch.*` entities with "light" in the name (e.g. `switch.switchtoiletlight`) | Tap a row to toggle on/off |
| **Temps** | Tint (drop) | All `sensor.*` entities with `device_class: temperature` | Read-only (value + unit) |
| **Doors** | Eye | All `binary_sensor.*` entities with `device_class: door`, `window`, or `garage_door` | Read-only (OPEN / CLOSED) |

Additional features:
- SNTP-synced clock in the header bar
- HA connection status indicator (green/yellow/red dot)
- Automatic WebSocket reconnection (5 s timeout, 30 s ping keepalive)
- Entity state updates arrive in real-time via `state_changed` event subscription

### Layout

```
+--------+------------------------------------------------------------+
|  NET   |  Header bar (HA status dot + label + clock)                |
|        +------------------------------------------------------------+
|  LGT   |                                                            |
|        |  Scrollable entity list                                    |
|  TEMP  |  (content changes with selected tab)                       |
|        |                                                            |
|  DOOR  |                                                            |
+--------+------------------------------------------------------------+
  100 px                      1180 px
```

---

## Hardware

| Component | Part | Notes |
|-----------|------|-------|
| SoC module | Waveshare ESP32-P4-Module-DEV-KIT-C | RISC-V dual-core @ 360 MHz, 16 MB flash, 32 MB octal PSRAM |
| Display | Waveshare 10.1-DSI-TOUCH-A | JD9365 panel, 800x1280 portrait, 2-lane MIPI-DSI. LVGL sw_rotate gives 1280x800 landscape |
| Touch | GT911 capacitive (I2C) | Shared I2C bus with backlight controller |
| Backlight | I2C controller at 0x45 | Reg 0x95 power gate, reg 0x96 brightness (0--255) |
| Network | RJ45 Ethernet (RMII) | Internal EMAC + IP101GRI PHY, DHCP |

### Display pipeline

The display is physically portrait (800x1280). Initialisation in `display.c`:

1. I2C bus on GPIO 7 (SDA) / GPIO 8 (SCL) at 400 kHz
2. Backlight controller power gate (reg 0x95: write 0x11 then 0x17), then brightness via reg 0x96
3. DSI PHY LDO enabled on internal channel 3 at 2500 mV
4. DSI bus: 2 lanes @ 1500 Mbps
5. BTA workaround applied (v1.x silicon hangs if `cmd_ack` is true)
6. JD9365 panel init with Waveshare-specific register overrides (200+ vendor commands)
7. GT911 touch on the same I2C bus
8. LVGL port registered with double-buffered partial rendering (100 lines/buffer in PSRAM, `sw_rotate` for landscape)

---

## Project structure

```
main/
  app_main.c          -- boot sequence: NVS -> Ethernet -> SNTP -> display -> UI -> HA client -> scanner -> 1 s timer
  config.h            -- all user-configurable values (network, HA, display, GPIO pins)
  display.c/h         -- MIPI-DSI panel + backlight + touch init, LVGL registration
  ha_client.c/h       -- WebSocket client: auth, get_states, subscribe_events, call_service
  net_scanner.c/h     -- ICMP /24 subnet scanner with reverse DNS
  ui/
    ui_dashboard.c/h  -- 4-tab LVGL dashboard (net, lights, temps, doors)
```

---

## Prerequisites

- [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/get-started/) installed (e.g. at `~/.espressif/v6.0.1/esp-idf/`)
- Python 3.x (3.14 confirmed working)
- Target chip set to `esp32p4`

---

## Configuration

All user-configurable values live in `main/config.h`. Edit before building.

### Home Assistant

```c
#define HA_HOST   "192.168.178.30"   // HA IP or hostname
#define HA_PORT   8123
#define HA_TOKEN  "eyJ..."           // Long-Lived Access Token
```

To generate a token: open HA -> click your profile -> Security -> Long-Lived Access Tokens -> Create Token.

### Ethernet PHY

```c
#define ETH_MDC_GPIO     GPIO_NUM_31
#define ETH_MDIO_GPIO    GPIO_NUM_52
#define ETH_PHY_ADDR     1
#define ETH_PHY_RST_GPIO GPIO_NUM_51
```

If your board uses a different PHY chip (LAN8720, RTL8201, etc.), change the `esp_eth_phy_new_*` call in `app_main.c`.

### Timezone

In `app_main.c`, update the POSIX TZ string:

```c
setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
```

See [posix_tz_db](https://github.com/nayarsystems/posix_tz_db) for other timezones.

---

## Building, reconfiguring, and flashing

All commands below assume ESP-IDF is sourced first:

```bash
source ~/.espressif/v6.0.1/esp-idf/export.sh
```

### Build

```bash
idf.py build
```

Dependencies declared in `main/idf_component.yml` are fetched automatically into `managed_components/` on first build.

### Reconfigure (menuconfig)

To change Kconfig options (PSRAM settings, LVGL memory, log levels, stack sizes, etc.):

```bash
idf.py menuconfig
```

This opens a TUI. Navigate with arrow keys, Enter to select, Esc to go back. Changes are saved to `sdkconfig`. Persistent defaults go in `sdkconfig.defaults` so they survive a `fullclean`.

To apply `sdkconfig.defaults` without the TUI (useful after editing `sdkconfig.defaults` by hand):

```bash
idf.py reconfigure
```

Notable Kconfig settings for this project (already set in `sdkconfig.defaults`):

| Setting | Value | Reason |
|---------|-------|--------|
| `CONFIG_SPIRAM` | `y` | Enable 32 MB PSRAM |
| `CONFIG_LV_MEM_CUSTOM` | `y` | LVGL allocates from system heap (PSRAM-backed) instead of tiny internal pool |
| `CONFIG_ESP_LVGL_PORT_TASK_STACK_SIZE` | `8192` | Larger stack for LVGL task |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | `65535` | Larger TCP buffers for WebSocket |

### Flash

```bash
idf.py -p /dev/ttyUSB0 flash
```

Replace `/dev/ttyUSB0` with your serial port (`/dev/ttyACM0`, `COM3`, etc.).

### Monitor (serial log output)

```bash
idf.py -p /dev/ttyUSB0 monitor
```

Press `Ctrl+]` to exit.

### Build + flash + monitor in one step

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

### Full clean rebuild

If you change `sdkconfig.defaults`, switch targets, or update managed components:

```bash
idf.py fullclean
idf.py build
```

### VS Code

If using the Espressif IDF extension:

1. `Ctrl+Shift+P` -> **ESP-IDF: Set Espressif Device Target** -> `esp32p4`
2. `Ctrl+Shift+P` -> **ESP-IDF: Select Port to Use** -> pick your serial port
3. Use the status bar buttons: Build (wrench), Flash (lightning), Monitor (screen)

---

## Dependencies

Declared in `main/idf_component.yml`, managed automatically:

| Component | Version | Purpose |
|-----------|---------|---------|
| `lvgl/lvgl` | 8.4.x | Graphics library |
| `espressif/esp_lvgl_port` | ^2 | LVGL task, display flush, touch input |
| `espressif/esp_lcd_jd9365` | ^2 | MIPI-DSI panel driver (Waveshare init override in display.c) |
| `espressif/esp_lcd_touch_gt911` | ^1 | Capacitive touch driver |
| `espressif/cjson` | ^1 | JSON parsing for HA WebSocket messages |
| `espressif/esp_websocket_client` | ^1 | WebSocket client for HA API |

Do not edit files under `managed_components/` -- they are fetched on build and not checked in.

---

## Architecture notes

### HA client threading model

The HA WebSocket client (`ha_client.c`) runs inside the `esp_websocket_client` event loop task. When entity data arrives (from `get_states` or `state_changed` events), it is parsed and delivered to the UI via `ui_dashboard_ha_update()`.

To avoid crashes, **LVGL widget creation is decoupled from the WebSocket callback**:

1. `ui_dashboard_ha_update()` only copies entity data into slot arrays (no LVGL calls, no locks)
2. A dirty flag (`s_ha_dirty`) is set when new data arrives
3. `ui_dashboard_tick_1s()` (called from a 1 s `esp_timer`) acquires the LVGL lock once, checks the flag, and creates/updates widgets in batches (max 10 new rows per tick)

This prevents LVGL heap exhaustion and stack overflow that would occur if hundreds of widgets were created directly from the WebSocket callback context.

### LVGL thread safety

All `lv_*` calls from outside the LVGL task must be wrapped in `lvgl_port_lock()` / `lvgl_port_unlock()`. The network scanner and HA connection status callbacks follow this pattern. Entity widget sync uses the batched approach described above.
