#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <driver/rmt.h>
#include <vector>
#include "portal_html.h"

#define SYMBOL_US        35
#define TOLERANCE        40
#define REFIRE_GUARD_MS  1500
#define PORTAL_HOLD_MS   3000

// ---- KineticSwitch -------------------------------------------------------------

class KineticSwitch {
public:
    String   name;
    String   url;
    uint32_t lastPressMs = 0;
    KineticSwitch(const char* n, const char* u = "") : name(n), url(u) {}
    virtual bool        match(const char* buf, int len) = 0;
    virtual const char* pattern() = 0;
};

class PatternSwitch : public KineticSwitch {
    String _pat;
public:
    PatternSwitch(const char* n, const char* p, const char* u = "") : KineticSwitch(n, u), _pat(p) {}
    const char* pattern() override { return _pat.c_str(); }
    bool match(const char* buf, int) override { return strstr(buf, _pat.c_str()) != nullptr; }
};

// ---- Config / NVS --------------------------------------------------------------

Preferences prefs;
String      wifi_ssid, wifi_pass;
std::vector<KineticSwitch*> switches;

void loadConfig() {
    prefs.begin("ks", true);
    wifi_ssid   = prefs.getString("ssid", "");
    wifi_pass   = prefs.getString("pass", "");
    String sw_j = prefs.getString("sw",   "[]");
    prefs.end();

    for (auto s : switches) {
        delete s;
    }
    
    switches.clear();
    JsonDocument doc;
    if (!deserializeJson(doc, sw_j) && doc.is<JsonArray>()) {
        for (JsonObject o : doc.as<JsonArray>()) {
            const char* n = o["n"]; const char* p = o["p"]; const char* u = o["u"];
            if (n && p) {
                switches.push_back(new PatternSwitch(n, p, u ? u : ""));
            }
        }

    // if (switches.empty()) {
    //     switches.push_back(new PatternSwitch("Switch A", "8e8e88e88888", ""));
    //     switches.push_back(new PatternSwitch("Switch B", "eeeee8888e8e8888", ""));
    //     switches.push_back(new PatternSwitch("Switch C", "eeeee888888ee888", ""));
    // }

    }
}

void saveConfig() {
    JsonDocument doc; JsonArray arr = doc.to<JsonArray>();
    for (auto s : switches) {
        JsonObject o = arr.add<JsonObject>();
        o["n"] = s->name.c_str();
        o["p"] = s->pattern();
        o["u"] = s->url.c_str();
    }
    String sw_j; serializeJson(doc, sw_j);
    prefs.begin("ks", false);
    prefs.putString("ssid", wifi_ssid);
    prefs.putString("pass", wifi_pass);
    prefs.putString("sw", sw_j);
    prefs.end();
}

// ---- CC1101 / RMT --------------------------------------------------------------

SPIClass spi(FSPI);
CC1101 radio = new Module(PIN_CS, PIN_GDO0, RADIOLIB_NC, PIN_GDO2, spi);

static uint8_t cc1101_read_status(uint8_t addr) {
    spi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    spi.transfer(addr | 0xC0);
    uint8_t v = spi.transfer(0);
    digitalWrite(PIN_CS, HIGH);
    spi.endTransaction();
    return v;
}

static void cc1101_write(uint8_t addr, uint8_t val) {
    spi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    spi.transfer(addr & 0x3F);
    spi.transfer(val);
    digitalWrite(PIN_CS, HIGH);
    spi.endTransaction();
}

#define MAX_NIB          128
#define RMT_RX_CHANNEL   RMT_CHANNEL_0
#define RMT_CLK_DIV      80
#define RMT_IDLE_US      500

volatile bool carrier_present = false;
RingbufHandle_t rmtRingBuf    = nullptr;

static inline char classify(uint32_t us) {
    if (us < 15) {
        return '?';
    }
    static const struct { uint8_t mult; char ch; } syms[] = {
        {1,'8'},{3,'e'},{4,'f'},{8,'F'}
    };
    char best = '?'; uint32_t best_err = UINT32_MAX;
    for (int i = 0; i < 4; i++) {
        uint32_t target = (uint32_t)SYMBOL_US * syms[i].mult;
        uint32_t tol    = target * TOLERANCE / 100;
        uint32_t err    = us > target ? us - target : target - us;
        if (err <= tol && err < best_err) {
            best = syms[i].ch;
            best_err = err;
        }
    }
    return best;
}

void IRAM_ATTR gdo2_cs_isr() {
    carrier_present = digitalRead(PIN_GDO2);
}

// ---- URL trigger ---------------------------------------------------------------

void callUrl(const String& url) {
    if (url.isEmpty()) {
        return;
    }
    HTTPClient http;
    if (url.startsWith("https")) {
        static WiFiClientSecure sc;
        sc.setInsecure();
        http.begin(sc, url);
    } else {
        http.begin(url);
    }
    Serial.printf("URL → %d\n", http.GET());
    http.end();
}

// ---- Burst processing ----------------------------------------------------------

void processBurst(char* expanded, int ei) {
    static int      last_sw      = -1;
    static uint32_t last_fire_ms = 0;
    for (int i = 0; i < (int)switches.size(); i++) {
        if (!switches[i]->match(expanded, ei)) {
            continue;
        }
        uint32_t now_ms = millis();
        if (i == last_sw && (now_ms - last_fire_ms) < REFIRE_GUARD_MS) {
            break;
        }
        switches[i]->lastPressMs = now_ms;
        Serial.printf(">>> %s\n", switches[i]->name.c_str());
        callUrl(switches[i]->url);
        last_sw = i; last_fire_ms = now_ms;
        break;
    }
}

bool tryProcessPacket() {
    if (!rmtRingBuf) {
        return false;
    }
    size_t rx_size;
    rmt_item32_t* items = (rmt_item32_t*)xRingbufferReceive(rmtRingBuf, &rx_size, 0);
    if (!items) {
        return false;
    }
    if (!carrier_present) {
        vRingbufferReturnItem(rmtRingBuf, items);
        return false;
    }

    int n = rx_size / sizeof(rmt_item32_t);
    char expanded[MAX_NIB * 2 + 2];
    int ei = 0;
    for (int j = 0; j < n && ei < (int)sizeof(expanded) - 2; j++) {
        uint32_t us = items[j].level0 ? items[j].duration0 : items[j].duration1;
        char c = classify(us);
        if (c == 'F') {
            expanded[ei++] = 'f';
            expanded[ei++] = 'f';
        } else if (c != '?') {
            expanded[ei++] = c;
        }
    }
    expanded[ei] = '\0';
    vRingbufferReturnItem(rmtRingBuf, items);

    if (ei >= 8) {
        processBurst(expanded, ei);
    }
    return true;
}

// ---- Captive portal ------------------------------------------------------------

WebServer server(80);

void handlePortalRoot() {
    int n = WiFi.scanNetworks();
    String opts;
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        String sel = (s == wifi_ssid) ? " selected" : "";
        opts += "<option value=\"" + s + "\"" + sel + ">" + s + " (" + WiFi.RSSI(i) + " dBm)</option>\n";
    }
    WiFi.scanDelete();
    JsonDocument doc; JsonArray arr = doc.to<JsonArray>();
    for (auto sw : switches) {
        JsonObject o = arr.add<JsonObject>();
        o["name"]    = sw->name.c_str();
        o["pattern"] = sw->pattern();
        o["url"]     = sw->url.c_str();
    }
    String swJson; serializeJson(doc, swJson);
    String html = String(PORTAL_HTML);
    html.replace("{{WIFI_OPTIONS}}", opts);
    html.replace("{{SSID}}", wifi_ssid);
    html.replace("{{PASS}}", wifi_pass);
    html.replace("{{SWITCHES_JSON}}", swJson);
    server.send(200, "text/html", html);
}

void handlePortalSave() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }
    
    JsonDocument doc;
    
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }
    wifi_ssid = doc["ssid"] | "";
    wifi_pass = doc["password"] | "";
    for (auto s : switches) {
        delete s;
    }
    switches.clear();
    for (JsonObject sw : doc["switches"].as<JsonArray>()) {
        const char* nm = sw["name"]; const char* pt = sw["pattern"]; const char* u = sw["url"];
        if (nm && pt && strlen(nm) && strlen(pt)) {
            switches.push_back(new PatternSwitch(nm, pt, u ? u : ""));
        }
    }
    saveConfig();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    delay(1000); ESP.restart();
}

void startCaptivePortal() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("KineticSwitch");
    delay(100);
    DNSServer dns;
    dns.start(53, "*", IPAddress(192, 168, 4, 1));
    server.on("/", HTTP_GET, handlePortalRoot);
    server.on("/save", HTTP_POST, handlePortalSave);
    server.onNotFound(handlePortalRoot);
    server.begin();
    Serial.println("Portal: SSID=KineticSwitch  192.168.4.1");
    while (true) {
        dns.processNextRequest();
        server.handleClient();
        tryProcessPacket();
        delay(5);
    }
}

// ---- Radio setup ---------------------------------------------------------------

void setupRadio() {
    spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    int state = radio.begin();
    Serial.printf("CC1101: %s (%d)\n", state == RADIOLIB_ERR_NONE ? "OK" : "FAIL", state);
    if (state != RADIOLIB_ERR_NONE) {
        while (1) {
            delay(100);
        }
    }
    radio.setFrequency(433.92);
    radio.setOOK(true);
    radio.setBitRate(28.6);
    radio.setRxBandwidth(200.0);
    state = radio.receiveDirectAsync();
    Serial.printf("receiveDirectAsync: %s (%d)\n", state == RADIOLIB_ERR_NONE ? "OK" : "FAIL", state);
    if (state != RADIOLIB_ERR_NONE) {
        while (1) {
            delay(100);
        }
    }
    cc1101_write(0x07, 0x06);
    cc1101_write(0x1B, 0xC7);
    cc1101_write(0x1C, 0x00);
    cc1101_write(0x1D, 0xB2);
    cc1101_write(0x00, 0x0E);

    rmt_config_t rmtCfg = {};
    rmtCfg.rmt_mode                      = RMT_MODE_RX;
    rmtCfg.channel                       = RMT_RX_CHANNEL;
    rmtCfg.gpio_num                      = (gpio_num_t)PIN_GDO0;
    rmtCfg.clk_div                       = RMT_CLK_DIV;
    rmtCfg.mem_block_num                 = 4;
    rmtCfg.rx_config.idle_threshold      = RMT_IDLE_US;
    rmtCfg.rx_config.filter_en           = true;
    rmtCfg.rx_config.filter_ticks_thresh = 10;
    rmt_config(&rmtCfg);
    rmt_driver_install(RMT_RX_CHANNEL, 4096, 0);
    rmt_get_ringbuf_handle(RMT_RX_CHANNEL, &rmtRingBuf);
    rmt_rx_start(RMT_RX_CHANNEL, true);

    pinMode(PIN_GDO2, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_GDO2), gdo2_cs_isr, CHANGE);
}

// ---- WiFi connect --------------------------------------------------------------

void wifiConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    Serial.print("WiFi: connecting");
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        Serial.print(".");
    }
    Serial.printf("\nWiFi up: %s\n", WiFi.localIP().toString().c_str());
}

// ---- Setup / Loop --------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);
    Serial.println("\n=== kinetic switch detector ===");

    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BOOT, INPUT_PULLUP);
    delay(100);

    loadConfig();
    setupRadio();

    bool enterPortal = wifi_ssid.isEmpty();
    if (!enterPortal && digitalRead(PIN_BOOT) == LOW) {
        Serial.printf("Boot held — hold %dms for portal\n", PORTAL_HOLD_MS);
        uint32_t held = millis();
        while (digitalRead(PIN_BOOT) == LOW && millis() - held < PORTAL_HOLD_MS) delay(50);
        enterPortal = (digitalRead(PIN_BOOT) == LOW);
    }
    
    if (enterPortal) {
        // never returns
        startCaptivePortal();
    }

    wifiConnect();
    Serial.println("Radio listening.");
}

void loop() {
    static int      peak_rssi = -120;
    static uint32_t last_hb   = 0;
    uint32_t now = millis();

    uint8_t rssi_raw = cc1101_read_status(0x34);
    int rssi_dbm = (rssi_raw >= 128) ? (rssi_raw - 256) / 2 - 74 : rssi_raw / 2 - 74;
    if (rssi_dbm > peak_rssi) {
        peak_rssi = rssi_dbm;
    }

    if (now - last_hb > 5000) {
        Serial.printf("[hb] peak=%ddBm carrier=%d\n", peak_rssi, (int)carrier_present);
        last_hb = now; peak_rssi = -120;
    }

    tryProcessPacket();

    static uint32_t boot_press_ms = 0;
    if (digitalRead(PIN_BOOT) == LOW) {
        if (!boot_press_ms) {
            boot_press_ms = millis();
        } else if (millis() - boot_press_ms > PORTAL_HOLD_MS) {
            Serial.println("BOOT held — entering portal");
            startCaptivePortal();
        }
    } else {
        boot_press_ms = 0;
    }
    

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi: lost, reconnecting");
        wifiConnect();
    }

    static uint32_t led_on_ms = 0;
    for (auto sw : switches) {
        if (sw->lastPressMs > led_on_ms) {
            led_on_ms = sw->lastPressMs;
        }
    }

    digitalWrite(PIN_LED, (led_on_ms && millis() - led_on_ms < 100) ? HIGH : LOW);
}
