# HA Dashboard

A Home Assistant dashboard running on the **Waveshare ESP32-P4-Module-DEV-KIT-C** with a **Waveshare 10.1" DSI-TOUCH-A** display. It connects to Home Assistant over WebSocket and renders a live, touch-interactive entity grid using LVGL.

## What it does

- Connects to your Wi-Fi network and Home Assistant instance
- Subscribes to all HA entity state changes via the WebSocket API
- Renders up to 32 entity cards (lights, switches, sensors, binary sensors, climate, covers) in a 3-column LVGL grid at 1280×800
- Tapping a card calls `toggle` / `turn_on` / `turn_off` on the corresponding HA entity
- Displays a live clock (SNTP-synced) and connection status indicator

## Hardware

| Component | Part |
|---|---|
| SoC module | Waveshare ESP32-P4-Module-DEV-KIT-C |
| Display | Waveshare 10.1-DSI-TOUCH-A (JD9365, 800×1280, MIPI-DSI) |
| Touch controller | GT911 (I2C) |
| Network | RJ45 Ethernet (RMII, internal EMAC + external PHY) |

## Project structure

```
main/
  app_main.c        — boot sequence: Ethernet → SNTP → display → LVGL → HA client
  config.h          — all user-configurable values (Wi-Fi, HA host/token, pins)
  display.c/h       — MIPI-DSI panel + backlight + touch init, LVGL registration
  ha_client.c/h     — WebSocket connection, auth, state subscription, service calls
  ui/
    ui_dashboard.c/h — LVGL dashboard screen (entity cards, clock, status bar)
```

## Prerequisites

- [ESP-IDF v6.0.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/get-started/)
- Python 3.x (3.14 confirmed working)
- VS Code with the Espressif IDF extension

---

## VS Code ESP-IDF extension setup

### 1. Install the extension

Open VS Code, go to the Extensions panel (`Ctrl+Shift+X`), search for **ESP-IDF** and install the extension published by Espressif Systems.

### 2. Run the setup wizard

Open the command palette (`Ctrl+Shift+P`) and run:

```
ESP-IDF: Configure ESP-IDF Extension
```

Choose **Express** setup. Set:

- **ESP-IDF version**: `v6.0.1` (or select "Find ESP-IDF in your system" if already installed)
- **ESP-IDF Tools directory**: `~/.espressif`

The wizard will download the toolchain and create the Python virtual environment. This takes a few minutes.

### 3. Select the target chip

Open the command palette and run:

```
ESP-IDF: Set Espressif Device Target
```

Select **esp32p4**.

Alternatively, from a terminal with the IDF environment sourced:

```bash
. ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py set-target esp32p4
```

### 4. Select the serial port

Connect the dev kit via USB, then run:

```
ESP-IDF: Select Port to Use
```

Pick the port for your board (e.g. `/dev/ttyUSB0` or `/dev/ttyACM0` on Linux).

---

## Project configuration

All user-configurable values live in **`main/config.h`**. Edit this file before building.

### Ethernet PHY

The board uses the ESP32-P4's internal EMAC with an external RMII PHY (defaulting to IP101). Verify the pins and PHY type against your board's schematic and update `config.h`:

```c
#define ETH_MDC_GPIO     GPIO_NUM_31   // MDIO clock
#define ETH_MDIO_GPIO    GPIO_NUM_52   // MDIO data
#define ETH_PHY_ADDR     1             // PHY address (0 or 1, check strap pins)
#define ETH_PHY_RST_GPIO GPIO_NUM_NC   // set if a reset GPIO is wired
```

If your board uses a different PHY chip (LAN8720, RTL8201, etc.), change the `esp_eth_phy_new_*` call in `app_main.c:eth_init()` accordingly. The network is configured via DHCP automatically.

### Home Assistant connection

```c
#define HA_HOST   "192.168.1.100"   // IP or hostname of your HA instance
#define HA_PORT   8123
#define HA_TOKEN  "eyJ..."          // Long-Lived Access Token (see below)
```

To generate a Long-Lived Access Token in Home Assistant:
1. Open your HA profile (click your username in the sidebar)
2. Scroll to **Security → Long-Lived Access Tokens**
3. Click **Create Token**, give it a name, copy the value into `HA_TOKEN`

### Timezone

In `app_main.c`, update the `TZ` environment variable to your timezone string:

```c
setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);   // Central Europe example
```

Standard POSIX timezone strings can be found at [https://github.com/nayarsystems/posix_tz_db](https://github.com/nayarsystems/posix_tz_db).

---

## Building and flashing

### From VS Code

Use the status bar buttons at the bottom of the window:

- **Build** (wrench icon) — compiles the project
- **Flash** (lightning icon) — flashes to the connected device
- **Monitor** (screen icon) — opens the serial monitor
- **Build, Flash and Monitor** — does all three in sequence

### From the terminal

```bash
. ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Press `Ctrl+]` to exit the monitor.

---

## Dependency notes

This project uses the IDF Component Manager. Dependencies are declared in `main/idf_component.yml` and downloaded automatically into `managed_components/` on first build. Do not edit files under `managed_components/` — they are not checked in and will be regenerated.

Key components:

| Component | Purpose |
|---|---|
| `espressif/esp_lcd_jd9365` | MIPI-DSI panel driver |
| `espressif/esp_lcd_touch_gt911` | Capacitive touch driver |
| `espressif/esp_lvgl_port` | LVGL task + display/touch integration |
| `lvgl/lvgl` | Graphics library (v8.4) |
| `espressif/esp_websocket_client` | WebSocket transport for HA API |
| `espressif/cjson` | JSON parsing for HA messages |
