#!/bin/bash
# Register this machine with the device (so it knows where to unicast status),
# then listen for the status messages it sends back on UDP 9001.
#
# Usage: ./udp-monitor.sh [hostname]   (default: esp32-spectral)

HOST="${1:-esp32-spectral}"
PORT=9001

# One-off datagram so the firmware learns our IP. -w1 keeps it from hanging.
printf 'hello' | nc -u -w1 "$HOST" "$PORT" 2>/dev/null \
    || echo "(couldn't reach $HOST:$PORT -- is the device up?)" >&2

echo "Listening for status on UDP $PORT (from $HOST)..."
nc -lup "$PORT"
