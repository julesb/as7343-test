# host/ — AS7343 visualiser

`as7343_viz.py` is a live terminal bar-chart visualiser for the sensor's OSC
stream. Pure Python 3 stdlib (no `pip install`), with a small built-in OSC
decoder.

```sh
./as7343_viz.py                 # device defaults to esp32-spectral
./as7343_viz.py 192.168.1.106   # or pass a hostname / IP
```

It registers with the device (sends a datagram to UDP 9001 so the firmware
learns where to stream), then listens on UDP 9000 and redraws on every frame.
Each band is coloured by its approximate visible wavelength; `Clear` is the
broadband channel. The header shows gain index, integration time, and a
saturation warning when a channel rails.

Quit with Ctrl-C (restores the cursor).

## Wire format

The firmware sends two OSC messages per sample to UDP `OSC_PORT` (9000):

- `/as7343/spectral` — 13 ints: F1, F2, FZ, F3, F4, F5, FY, FXL, F6, F7, F8,
  NIR (ascending wavelength), then Clear (broadband, averaged).
- `/as7343/meta` — `gain_idx` (int), `integration_ms` (float),
  `analog_saturated` (int 0/1), `digital_saturated` (int 0/1).

Any tool that speaks OSC can consume these; the Python script is just one
convenient consumer.
