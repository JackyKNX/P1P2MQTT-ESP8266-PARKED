# P1P2MQTT-ESP8266-PARKED

> Minimal ESP8266 companion firmware for the P1P2MQTT migration to ESP32.

**Status: Parked / Legacy**

This project is retained only as a parked legacy companion to the active
**P1P2MQTT-ESP32** project.

## Purpose

This is a minimal ESP8266 firmware created during the migration from the
legacy ESP8266 Wi-Fi bridge to the new ESP32 Ethernet + PoE gateway.

The ESP8266 is no longer used as the network/MQTT bridge.

Its only purpose in this configuration is to keep the existing ATmega328P /
P1P2Monitor operating correctly while the ESP32 takes over the network and
MQTT functions.

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

The ESP32 is now the active network gateway.

## What this firmware does

The firmware intentionally contains no MQTT, web server or P1/P2 protocol
implementation.

Its P1P2-related function is limited to maintaining the required:

```text
ATMEGA_SERIAL_ENABLE
```

state so that the ATmega328P/P1P2Monitor can continue operating normally.

The ESP8266 does **not** communicate with the ATmega UART.

## Why keep it?

This minimal firmware provides a safe way to leave the old ESP8266 connected
during the migration without allowing it to interfere with the new ESP32
gateway.

It can also be useful as a simple fallback or test firmware for the old
ESP8266 hardware.

## Replacement

The ESP8266 bridge is replaced by the active
**P1P2MQTT-ESP32** Ethernet + PoE gateway:

https://github.com/JackyKNX/P1P2MQTT-ESP32

The ESP32 provides:

- Ethernet instead of Wi-Fi
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

This project is **parked** and is not intended for further feature
development.

The active development target is the **P1P2MQTT-ESP32** Ethernet + PoE
platform.
