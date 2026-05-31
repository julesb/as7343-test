#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include "config.h"

// Phase 1 scaffold: WiFi + OTA + periodic UDP status messages.
// No sensor yet -- this just confirms the board flashes, joins WiFi,
// accepts OTA updates, and that we can see it from the host with nc.

WiFiUDP udp;
IPAddress statusDest;   // host to send status to; learned from incoming packets

// Print to serial and, once a host has registered, unicast to it on LOG_PORT.
// We unicast to a learned destination rather than broadcast: OpenBSD nc (the
// Debian default) connect()s to the first sender in UDP listen mode, after
// which it only accepts unicast to this host's address -- broadcasts would be
// dropped after the first packet. The host registers by sending us any UDP
// datagram on LOG_PORT (see udp-monitor.sh).
void udpLog(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial.print(buf);

    if (statusDest && WiFi.status() == WL_CONNECTED) {
        udp.beginPacket(statusDest, LOG_PORT);
        udp.print(buf);
        udp.endPacket();
    }
}

void connectWiFi() {
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");  // no WiFi yet, serial only
    }
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
    Serial.begin(115200);
    delay(500);

    connectWiFi();

    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.onStart([]() { udpLog("OTA update starting...\n"); });
    ArduinoOTA.onEnd([]()   { udpLog("OTA done!\n"); });
    ArduinoOTA.onError([](ota_error_t error) { udpLog("OTA error [%u]\n", error); });
    ArduinoOTA.begin();

    udp.begin(LOG_PORT);   // listen for host registration packets

    udpLog("Booted '%s' fw=%s ip=%s\n", OTA_HOSTNAME, FW_VERSION,
           WiFi.localIP().toString().c_str());
}

unsigned long lastStatus = 0;

void loop() {
    ArduinoOTA.handle();

    // Reconnect if WiFi drops so OTA/status keep working.
    if (WiFi.status() != WL_CONNECTED) {
        udpLog("WiFi lost, reconnecting...\n");
        connectWiFi();
    }

    // Any incoming datagram registers (or updates) the status destination.
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
        IPAddress src = udp.remoteIP();
        while (udp.available()) udp.read();   // drain payload, we don't need it
        if (statusDest != src) {
            statusDest = src;
            udpLog("status dest -> %s:%d\n", statusDest.toString().c_str(), LOG_PORT);
        }
    }

    unsigned long now = millis();
    if (now - lastStatus >= STATUS_INTERVAL_MS) {
        lastStatus = now;
        udpLog("[status] up=%lus ip=%s rssi=%ddBm heap=%u\n",
               now / 1000,
               WiFi.localIP().toString().c_str(),
               WiFi.RSSI(),
               (unsigned)ESP.getFreeHeap());
    }
}
