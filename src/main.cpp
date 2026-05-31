#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include "config.h"

// Phase 1 scaffold: WiFi + OTA + periodic UDP status broadcast.
// No sensor yet -- this just confirms the board flashes, joins WiFi,
// accepts OTA updates, and that we can see it from the host with nc.

WiFiUDP udp;
IPAddress broadcastIP;

// Print to serial and broadcast over UDP to LOG_PORT. We broadcast (rather
// than unicast to a known host) so any machine on the LAN running
// `nc -lup 9001` receives the messages without a prior handshake.
void udpLog(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial.print(buf);

    if (WiFi.status() == WL_CONNECTED) {
        udp.beginPacket(broadcastIP, LOG_PORT);
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

    // Derive the subnet broadcast address (e.g. 192.168.1.255).
    broadcastIP = WiFi.localIP();
    broadcastIP[3] = 255;

    Serial.printf("\nConnected! IP: %s  broadcast: %s\n",
                  WiFi.localIP().toString().c_str(),
                  broadcastIP.toString().c_str());
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
