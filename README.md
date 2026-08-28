# P1P2MQTT-ESP8266-PARKED

> Minimal ESP8266 companion firmware for the P1P2MQTT migration to ESP32.

**Status: Parked / Legacy**

This project is retained as a legacy companion to the active
**P1P2MQTT-ESP32** project.

## Purpose

The ESP8266 is no longer the network/MQTT bridge. The active gateway is the
ESP32 Ethernet + PoE platform.

The ESP8266 firmware keeps the existing ATmega328P/P1P2Monitor installation
operational while the ESP32 handles the network and MQTT functions.

```text
Legacy:

ATmega328P / P1P2Monitor
        |
        +---- ESP8266 Wi-Fi ---- MQTT


Current:

ATmega328P / P1P2Monitor
        |
        +---- ESP32 Ethernet + PoE ---- MQTT
```

**The ESP8266 does not communicate with the ATmega UART.**

Its P1P2-related function is limited to maintaining:

```text
ATMEGA_SERIAL_ENABLE
```

## Management features

Although the firmware is intentionally minimal, it provides basic local
management and diagnostics:

- WiFi configuration with a dedicated setup AP
- WiFi network scanning and selection
- saved WiFi configuration retained across firmware updates
- automatic WiFi reconnect
- fallback to the configuration AP when WiFi remains unavailable
- ESP8266 restart from the web interface
- web-based firmware update (OTA)
- ArduinoOTA
- system log
- basic system diagnostics
- free heap
- uptime
- MAC / chip ID
- boot counter
- reset reason
- WiFi signal/status information
- JSON status endpoint

### First-time WiFi setup

If no usable WiFi connection is available, the ESP8266 starts its own
configuration network:

```text
SSID:     P1P2-Setup-XXXXXX
Password: p1p2setup
IP:       192.168.4.1
```

Open:

```text
http://192.168.4.1/
```

and use **WiFi configuration** to select the target network and enter its
password.

After a successful connection, the ESP8266 leaves the configuration AP and
operates as a parked companion device.

## Web interface

The web interface provides:

- current WiFi status
- system diagnostics
- WiFi configuration
- WiFi scan
- system log
- firmware update
- ESP8266 restart

The main page also provides links to the two related repositories:

- **Active ESP32 gateway:** https://github.com/JackyKNX/P1P2MQTT-ESP32
- **This ESP8266 companion:** https://github.com/JackyKNX/P1P2MQTT-ESP8266-PARKED

## Why keep it?

This firmware provides a safe way to keep the old ESP8266 hardware installed
during and after the migration without allowing it to interfere with the new
ESP32 gateway.

It can also be used as a simple fallback or test firmware for the legacy
ESP8266 hardware.

## Replacement

The ESP8266 bridge is replaced by:

**P1P2MQTT-ESP32 — Ethernet + PoE gateway**

https://github.com/JackyKNX/P1P2MQTT-ESP32

The ESP32 provides:

- Ethernet instead of WiFi
- IEEE 802.3af PoE
- MQTT
- web diagnostics
- OTA
- native openHAB MQTT integration
- Home Assistant discovery compatibility

## Related project

The original P1P2MQTT implementation by Arnold Niessen:

https://github.com/Arnold-n/P1P2MQTT

## Status

This project is **parked** and is not intended to become another P1P2
network gateway.

Future development should primarily target the
**P1P2MQTT-ESP32 Ethernet + PoE** platform.

The ESP8266 project is maintained only for legacy hardware, basic management
and migration/fallback purposes.
