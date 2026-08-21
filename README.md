# P1P2MQTT-ESP8266-PARKED

Minimal, standalone "parking" firmware for the ESP8266 that used to run
Arnold Niessen's [P1P2-bridge-esp8266](https://github.com/Arnold-n/P1P2MQTT)
on the combined ESP8266+ATmega328P board.

## Why this exists

This ESP8266 was replaced by an ESP32 running
[P1P2MQTT-ESP32](https://github.com/JackyKNX/P1P2MQTT-ESP32) (or your
fork's URL), which now handles all P1/P2 MQTT/Home Assistant duties
directly over its own UART link to the same ATmega.

The ESP8266 is still physically present on the board (soldered, not
removed) and its UART TX pin is wired to the same net as the ATmega's
RX pin that the ESP32 now also drives. **Two live UART transmitters on
one line is not safe** — so instead of leaving Arnold's original
firmware running (which would actively transmit), this repo provides a
minimal replacement that deliberately does nothing on that line, while
keeping the ESP8266 remotely manageable.

## What this firmware does

- **Never** calls `Serial.begin()` and never touches GPIO1 (TX) /
  GPIO3 (RX) — the pins wired to the ATmega — for any purpose. Contains
  no Arnold / P1P2Monitor / P1P2Serial code at all.
- Connects to WiFi (hardcoded SSID/password in `main.cpp`).
- Serves a small web UI:
  - `/` — status page (firmware version, IP, RSSI, free heap, uptime)
  - `/serial` — a web log viewer for this device's **own** diagnostics
    (WiFi/OTA events) — **not** a P1/P2 bus monitor, since this
    firmware never touches that UART.
  - `/update` — password-protected browser-based OTA upload
    (`ESP8266HTTPUpdateServer`)
- Also exposes ArduinoOTA (IDE/`espota`-based upload) as a second update
  path, same password.

## The one P1P2-related thing it *does* do: `ATMEGA_SERIAL_ENABLE`

Arnold's board design uses a separate control line — ESP8266 **GPIO15**,
wired to the ATmega's **PD4** pin — that must be driven **HIGH** for
P1P2Monitor (the ATmega firmware) to enable its own serial input/output
at all. This is *not* part of the UART TX/RX data path; it's a simple
enable signal.

Arnold's original firmware sets this at boot:

```cpp
#define ATMEGA_SERIAL_ENABLE 15 // required for v1.2
...
digitalWrite(ATMEGA_SERIAL_ENABLE, HIGH);
pinMode(ATMEGA_SERIAL_ENABLE, OUTPUT);
```

**This firmware replicates only that one line** (`setup()`, right at the
top). Without it, the ATmega stays completely silent on the bus, even
though nothing is wrong with the UART wiring — found the hard way during
bring-up. See `main.cpp` for the exact code and comments.

If your board doesn't use this control line (only relevant for "bridge
v1.2" per Arnold's comment), you can remove this without affecting the
rest of the firmware.

## Building and flashing

```bash
pio run
```

Produces `.pio/build/esp8266_parked/firmware.bin`.

**First flash (no USB needed):** if the ESP8266 is still running Arnold's
original firmware, it already exposes an *unauthenticated* browser
upload at `http://<esp8266-ip>/update` (confirmed: Arnold's
`P1P2MQTT-bridge.ino` calls `httpUpdater.setup(&httpServer)` with no
username/password). Open that URL, upload the `.bin` directly.

**Subsequent updates:** use this firmware's own `/update` (now
password-protected) or ArduinoOTA.

## Before flashing — change these in `main.cpp`

```cpp
static const char *WIFI_SSID     = "...";
static const char *WIFI_PASSWORD = "...";

static const char *OTA_USER     = "admin";
static const char *OTA_PASSWORD = "...";   // >>> CHANGE ME <<<
```

Credentials are hardcoded in plain text in the source/binary — treat
`/update` and the OTA port as LAN-only, same as the ESP32 project.

## Related

- [P1P2MQTT-ESP32](https://github.com/JackyKNX/P1P2MQTT-ESP32) — the
  ESP32 firmware this board now runs for actual P1/P2 MQTT/HA duties.
- [P1P2MQTT](https://github.com/Arnold-n/P1P2MQTT) — Arnold Niessen's
  original project, whose ESP8266 firmware this repo replaces on this
  specific board.
