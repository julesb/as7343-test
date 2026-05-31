# as7343-test

ESP32-S3 firmware for the [Adafruit AS7343](https://www.adafruit.com/product/6477)
14-channel spectral sensor (I2C). Host-side tooling for visualising sensor
readings over OSC.

## Status

**Phase 1, step 1 — scaffold (current).** WiFi + OTA + a periodic UDP status
broadcast. No sensor code yet; this exists to confirm the board flashes, joins
WiFi, takes OTA updates, and is reachable from the host before we wire up I2C.

Roadmap:
- **Phase 1:** AS7343 firmware reading the 14 spectral channels and streaming
  them over OSC; a small host program to receive and visualise the values.
- **Phase 2:** RGB laser control + a calibration routine that drives the laser,
  reads the sensor, and builds a Quicksilver colour-calibration profile.

## Hardware

- ESP32-S3-DevKitC-1
- Adafruit AS7343 breakout (VIN, GND, SCL, SDA, GPIO, INT) — I2C, default
  address `0x39`. Wiring TBD when sensor code lands.

## Setup

```sh
cp src/config.h.example src/config.h   # then edit WiFi credentials
```

`src/config.h` is gitignored (holds WiFi credentials).

## Build & flash

First flash must be over USB (OTA needs OTA-capable firmware already running):

```sh
pio run -e esp32s3 -t upload     # serial, /dev/ttyACM0
```

Subsequent flashes over the air:

```sh
./ota-upload.sh                  # = pio run -e ota -t upload
```

## Monitoring

The firmware broadcasts a status line on UDP port 9001 every couple of seconds.
Capture it on the host with:

```sh
./udp-monitor.sh                 # = nc -lup 9001
```

Expected output:

```
Booted 'as7343' fw=0.1.0-scaffold ip=192.168.x.y
[status] up=2s ip=192.168.x.y rssi=-52dBm heap=...
```
