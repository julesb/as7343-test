#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Adafruit_AS7343.h>
#include <OSCMessage.h>
#include <math.h>

#include "config.h"

// Phase 1, step 2: read the AS7343 spectral channels and stream them over OSC.
// Still keeps the WiFi + OTA + UDP status scaffold from step 1.

WiFiUDP udp;
IPAddress statusDest;        // host to send status/OSC to; learned from incoming packets
Adafruit_AS7343 as7343;
bool sensorOk = false;

// Runtime-tunable settings (seeded from config.h, then overridable via OSC).
// Gain has to be tracked here because the chip has no read-back for it, and we
// report it in /as7343/meta. Integration time we can read back from the chip.
int currentGain = AS7343_GAIN;

// Integration time is set as a millisecond value over OSC and converted to the
// chip's ATIME/ASTEP pair. We hold ASTEP fixed at the library default (599 ->
// 1.668 ms/step) and vary ATIME: that keeps each step's max count past the
// 16-bit ADC ceiling, so the full dynamic range is preserved at any IT.
static const uint16_t ASTEP_FIXED = 599;
static const float    STEP_MS     = (ASTEP_FIXED + 1) * 0.00278f;  // ~1.668 ms

// Spectral bands in ascending-wavelength order. `idx` is the band's position in
// the 18-entry buffer returned by readAllChannels() in auto-SMUX 18-channel mode.
struct Band { const char* name; uint16_t nm; uint8_t idx; };
static const Band BANDS[] = {
    {"F1",  405, AS7343_CHANNEL_F1},
    {"F2",  425, AS7343_CHANNEL_F2},
    {"FZ",  450, AS7343_CHANNEL_FZ},
    {"F3",  475, AS7343_CHANNEL_F3},
    {"F4",  515, AS7343_CHANNEL_F4},
    {"F5",  550, AS7343_CHANNEL_F5},
    {"FY",  555, AS7343_CHANNEL_FY},
    {"FXL", 600, AS7343_CHANNEL_FXL},
    {"F6",  640, AS7343_CHANNEL_F6},
    {"F7",  690, AS7343_CHANNEL_F7},
    {"F8",  745, AS7343_CHANNEL_F8},
    {"NIR", 855, AS7343_CHANNEL_NIR},
};
static const uint8_t NUM_BANDS = sizeof(BANDS) / sizeof(BANDS[0]);

// VIS/Clear is measured 6 times across the SMUX cycles; average for one value.
static const uint8_t VIS_IDX[] = {
    AS7343_CHANNEL_VIS_TL_0, AS7343_CHANNEL_VIS_BR_0,
    AS7343_CHANNEL_VIS_TL_1, AS7343_CHANNEL_VIS_BR_1,
    AS7343_CHANNEL_VIS_TL_2, AS7343_CHANNEL_VIS_BR_2,
};

// Latest readings, kept for the periodic status line.
uint16_t lastBuf[18] = {0};
uint16_t lastClear = 0;

// Print to serial and, once a host has registered, unicast to it on LOG_PORT.
// The host registers by sending us any UDP datagram on LOG_PORT (the OSC stream
// goes to the same learned IP on OSC_PORT). See udp-monitor.sh / host/.
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

void initSensor() {
    Wire.begin(I2C_SDA, I2C_SCL);

    // INT/GPIO pins are wired but unused for now; park INT with a pull-up so it
    // doesn't float (the AS7343 INT output is open-drain, active low).
    pinMode(AS7343_INT_PIN, INPUT_PULLUP);

    if (!as7343.begin()) {
        sensorOk = false;
        udpLog("AS7343 NOT found on I2C (SDA=%d SCL=%d) -- check wiring\n",
               I2C_SDA, I2C_SCL);
        return;
    }

    as7343.setSMUXMode(AS7343_SMUX_18CH);          // all spectral channels
    as7343.setGain((as7343_gain_t)currentGain);

    sensorOk = true;
    udpLog("AS7343 ok: id=0x%02X gain_idx=%d intTime=%.1fms\n",
           as7343.getPartID(), currentGain, as7343.getIntegrationTime());
}

void sendOSC(const char* addr, OSCMessage& msg) {
    udp.beginPacket(statusDest, OSC_PORT);
    msg.send(udp);
    udp.endPacket();
    msg.empty();
}

// --- Incoming OSC control (push from host) -------------------------------
// Read a numeric argument as float, accepting either OSC int or float types.
// Returns false if arg `i` is missing or non-numeric.
static bool oscNum(OSCMessage& msg, int i, float& out) {
    if (i >= msg.size()) return false;
    if (msg.isFloat(i))    { out = msg.getFloat(i); return true; }
    if (msg.isInt(i))      { out = (float)msg.getInt(i); return true; }
    return false;
}

// /as7343/set/gain <gain_idx 0..12>  -- analog gain, 0.5x..2048x
void oscSetGain(OSCMessage& msg) {
    float v;
    if (!sensorOk || !oscNum(msg, 0, v)) return;
    int g = (int)lroundf(v);
    if (g < 0)  g = 0;
    if (g > 12) g = 12;
    as7343.setGain((as7343_gain_t)g);
    currentGain = g;
    udpLog("set gain -> idx=%d\n", g);
}

// /as7343/set/inttime <ms>  -- integration time in milliseconds. Quantised to
// the ASTEP step (~1.668 ms); range ~1.7..427 ms (ATIME 0..255).
void oscSetIntTime(OSCMessage& msg) {
    float ms;
    if (!sensorOk || !oscNum(msg, 0, ms)) return;
    long atime = lroundf(ms / STEP_MS) - 1;
    if (atime < 0)   atime = 0;
    if (atime > 255) atime = 255;
    as7343.setASTEP(ASTEP_FIXED);
    as7343.setATIME((uint8_t)atime);
    udpLog("set inttime -> req=%.1fms actual=%.1fms (atime=%ld astep=%d)\n",
           ms, as7343.getIntegrationTime(), atime, ASTEP_FIXED);
}

// Parse a received datagram as OSC and route control messages. Non-OSC packets
// (e.g. plain host-registration datagrams) fail to parse and are ignored here.
void handleIncomingOSC(uint8_t* data, int len) {
    OSCMessage msg;
    msg.fill(data, len);
    if (msg.hasError()) return;
    msg.dispatch("/as7343/set/gain",    oscSetGain);
    msg.dispatch("/as7343/set/inttime", oscSetIntTime);
}

void readAndStream() {
    uint16_t buf[18];
    if (!as7343.readAllChannels(buf)) {
        udpLog("AS7343 read failed/timeout\n");
        return;
    }

    uint32_t visSum = 0;
    for (uint8_t i = 0; i < 6; i++) visSum += buf[VIS_IDX[i]];
    uint16_t clear = visSum / 6;

    memcpy(lastBuf, buf, sizeof(lastBuf));
    lastClear = clear;

    if (!statusDest) return;   // nobody listening yet

    // /as7343/spectral <F1..NIR ints, wavelength order> <clear int>
    OSCMessage spectral("/as7343/spectral");
    for (uint8_t i = 0; i < NUM_BANDS; i++) spectral.add((int32_t)buf[BANDS[i].idx]);
    spectral.add((int32_t)clear);
    sendOSC("/as7343/spectral", spectral);

    // /as7343/raw <18 ints> -- unmapped buffer in library/SMUX order, for
    // diagnosing the channel mapping (host/as7343_viz.py --raw).
    OSCMessage raw("/as7343/raw");
    for (uint8_t i = 0; i < 18; i++) raw.add((int32_t)buf[i]);
    sendOSC("/as7343/raw", raw);

    // /as7343/meta <gain_idx int> <intTime ms float> <analogSat int> <digitalSat int>
    OSCMessage meta("/as7343/meta");
    meta.add((int32_t)currentGain);
    meta.add(as7343.getIntegrationTime());
    meta.add((int32_t)as7343.isAnalogSaturated());
    meta.add((int32_t)as7343.isDigitalSaturated());
    sendOSC("/as7343/meta", meta);
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

    initSensor();
}

unsigned long lastSample = 0;
unsigned long lastStatus = 0;

void loop() {
    ArduinoOTA.handle();

    if (WiFi.status() != WL_CONNECTED) {
        udpLog("WiFi lost, reconnecting...\n");
        connectWiFi();
    }

    // Any incoming datagram registers (or updates) the status/OSC destination;
    // it's also parsed as OSC so the host can push /as7343/set/* control here.
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
        IPAddress src = udp.remoteIP();
        uint8_t pkt[128];
        int len = udp.read(pkt, sizeof(pkt));
        if (statusDest != src) {
            statusDest = src;
            udpLog("dest -> %s (status:%d osc:%d)\n",
                   statusDest.toString().c_str(), LOG_PORT, OSC_PORT);
        }
        if (len > 0) handleIncomingOSC(pkt, len);
    }

    unsigned long now = millis();

    if (sensorOk && now - lastSample >= SAMPLE_INTERVAL_MS) {
        lastSample = now;
        readAndStream();
    }

    if (now - lastStatus >= STATUS_INTERVAL_MS) {
        lastStatus = now;
        if (sensorOk) {
            // Compact human-readable spectral snapshot for udp-monitor.sh.
            udpLog("[status] up=%lus rssi=%ddBm heap=%u clear=%u "
                   "F1=%u FZ=%u F4=%u FY=%u F6=%u NIR=%u\n",
                   now / 1000, WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), lastClear,
                   lastBuf[AS7343_CHANNEL_F1], lastBuf[AS7343_CHANNEL_FZ],
                   lastBuf[AS7343_CHANNEL_F4], lastBuf[AS7343_CHANNEL_FY],
                   lastBuf[AS7343_CHANNEL_F6], lastBuf[AS7343_CHANNEL_NIR]);
        } else {
            udpLog("[status] up=%lus rssi=%ddBm heap=%u sensor=MISSING\n",
                   now / 1000, WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
        }
    }
}
