# as7343-test

ESP32-S3 firmware for the [Adafruit AS7343](https://www.adafruit.com/product/6477)
14-channel spectral sensor (I2C). Host-side tooling for visualising sensor
readings over OSC.

## Status

**Phase 1, step 2 — sensor + OSC (current).** Reads the AS7343 spectral
channels and streams them over OSC, plus the WiFi/OTA/UDP-status scaffold from
step 1. A host-side terminal visualiser lives in `host/`.

Roadmap:
- **Phase 1:** AS7343 firmware reading the spectral channels and streaming them
  over OSC; a small host program to receive and visualise the values.
- **Phase 2:** RGB laser control + a calibration routine that drives the laser,
  reads the sensor, and builds a Quicksilver colour-calibration profile.

## Hardware

- ESP32-S3-DevKitC-1
- Adafruit AS7343 breakout — I2C, default address `0x39`

Wiring (pins set in `config.h`, all remappable via the GPIO matrix):

| Sensor | ESP32-S3 |
|--------|----------|
| VIN    | 3V3      |
| GND    | GND      |
| SDA    | GPIO8    |
| SCL    | GPIO9    |
| INT    | GPIO17 *(wired, unused for now)* |
| GPIO   | GPIO18 *(wired, unused for now)* |

## Sensor stream (OSC)

The firmware unicasts two OSC messages per sample to UDP `OSC_PORT` (9000) on
the registered host:

- `/as7343/spectral` — 13 ints: F1, F2, FZ, F3, F4, F5, FY, FXL, F6, F7, F8,
  NIR (ascending wavelength), then Clear (broadband).
- `/as7343/meta` — gain index (int), integration time ms (float), analog
  saturated (0/1), digital saturated (0/1).

Visualise with the host tool (no dependencies):

```sh
host/as7343_viz.py               # or: host/as7343_viz.py <hostname|ip>
```

See `host/README.md` for details. Gain and sample rate are tunable in
`config.h` (`AS7343_GAIN`, `SAMPLE_INTERVAL_MS`); gain and integration time are
also tunable live over OSC (below).

## Control (OSC)

The host pushes control messages to the device's UDP `LOG_PORT` (9001) — the
same port it registers on. Changes take effect on the next sample and are
echoed back in `/as7343/meta`.

- `/as7343/set/gain <int>` — analog gain index, `0..12` (0.5×…2048×), clamped.
  Default `7` (64×), from `AS7343_GAIN` in `config.h`.
- `/as7343/set/inttime <float|int>` — integration time in **milliseconds**,
  ~1.7…427 ms. Quantised to the ~1.668 ms ASTEP step (ASTEP held at 599 to keep
  the full 16-bit dynamic range; ATIME is varied). Default ~50 ms (the library
  boot default; not set in firmware).

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

The firmware unicasts a status line on UDP port 9001 every couple of seconds,
but only once a host has registered. `udp-monitor.sh` registers (sends one
datagram so the device learns our IP) and then listens:

```sh
./udp-monitor.sh                 # or: ./udp-monitor.sh <hostname>
```

Expected output (a new line every ~2s). The status line carries a few spectral
channels too, so this is the quickest way to confirm the sensor works before
reaching for the visualiser:

```
dest -> 192.168.x.y (status:9001 osc:9000)
AS7343 ok: id=0x81 gain_idx=7 intTime=50.0ms
[status] up=2s rssi=-52dBm heap=... clear=1234 F1=12 FZ=89 F4=210 FY=350 F6=140 NIR=20
```

If the sensor isn't detected you'll see `sensor=MISSING` instead — check wiring.

We unicast to a learned host rather than broadcast because OpenBSD `nc` (the
Debian default) locks onto the first sender in UDP listen mode and then drops
broadcast packets, so only one would ever arrive.

We unicast to a learned host rather than broadcast because OpenBSD `nc` (the
Debian default) locks onto the first sender in UDP listen mode and then drops
broadcast packets, so only one would ever arrive.
