#include "classroom_network.h"
#include "classroom_safety.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <esp_random.h>
#include <esp_system.h>
#include <algorithm>
#include <atomic>
#include <time.h>

#ifndef CLASSROOM_TEAM
#define CLASSROOM_TEAM "DEMO"
#endif

namespace {
constexpr char Endpoint[] = "https://sjjhysopzecqczutuelu.supabase.co/functions/v1/classroom-ingest";
constexpr unsigned Capacity = 96;
#ifdef CLASSROOM_LEVEL2_ONLY
constexpr char Firmware[] = "level2";
#else
constexpr char Firmware[] = "level3";
#endif
struct Record { char body[850]; };
struct Urgent { const char* kind; ClassroomSafety safety; time_t captured; uint32_t uptime; };
struct Settings {
    char ssid[33], password[65], room[33], device[37], token[65];
};
Settings config{};
WebServer web(80);
Preferences preferences;
QueueHandle_t pending;
QueueHandle_t uploadConfig;
QueueHandle_t latestCard;
QueueHandle_t urgentKinds;
QueueHandle_t urgentRecords;
struct Card { char uid[21]; };
char boot[37];
unsigned sequence;
std::atomic<unsigned> lastUpload{0}, drops{0}, scanAt{0};
std::atomic<int> wifiState{0}, cloudCode{0}, role{0};
std::atomic<bool> setupRequested{false}, resendRequested{false}, inFlight{false};
bool provisioning, submitted;
uint32_t attemptAt;
char hostname[24];

void uuid(char* out) {
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 15) | 64;
    b[8] = (b[8] & 63) | 128;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}

bool safeCode(const String& value, unsigned size, bool hex = false) {
    if (!value.length() || value.length() > size) return false;
    for (unsigned i = 0; i < value.length(); ++i) {
        const unsigned char c = value[i];
        if (hex ? !isxdigit(c) : !(isalnum(c) || c == '-' || c == '_')) return false;
    }
    return true;
}

bool validUuid(const String& value) {
    if (value.length() != 36) return false;
    for (unsigned i = 0; i < 36; ++i) {
        const unsigned char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!isxdigit(c)) {
            return false;
        }
    }
    return true;
}

void startSetup() {
    if (provisioning) return;
    char password[17];
    snprintf(password, sizeof(password), "%08lx%08lx",
             static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    const String ssid = String("SmartClass-") + CLASSROOM_TEAM;
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(ssid.c_str(), password)) {
        Serial.println("WIFI SETUP: AP START FAILED");
        return;
    }
    provisioning = true;
    Serial.printf("WIFI SETUP: %s password=%s http://192.168.4.1\n", ssid.c_str(), password);
}

bool setupClient() {
    return provisioning && web.client().localIP() == WiFi.softAPIP() &&
           web.hostHeader() == "192.168.4.1";
}

void setupPage() {
    if (!setupClient()) {
        web.send(404);
        return;
    }
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "text/html", R"HTML(<!doctype html><meta name="viewport" content="width=device-width">
<title>Smart Classroom setup</title><h1>Connect Smart Classroom</h1>
<form method="post" action="/setup" autocomplete="off">
<p><label>Wi-Fi name <input name="ssid" required maxlength="32"></label></p>
<p><label>Wi-Fi password <input type="password" name="password" maxlength="64"></label></p>
<p><label>Room code <input name="room" required maxlength="32"></label></p>
<p><label>Device UUID <input name="device" required maxlength="36"></label></p>
<p><label>Device token <input type="password" name="token" required maxlength="64"></label></p>
<button>Connect</button></form><p><a href="/demo/card">Private card demo</a></p>
<p><a href="/status">Classroom status</a></p>)HTML");
}

void saveSetup() {
    if (!setupClient() || web.header("Origin") != "http://192.168.4.1") {
        web.send(403);
        return;
    }
    // Drain under the old identity before allowing reassignment.
    if (config.device[0] && (web.arg("device") != config.device || web.arg("room") != config.room)) {
        web.send(409, "text/plain", "Device and room are fixed after provisioning.");
        return;
    }
    const String ssid = web.arg("ssid"), password = web.arg("password"), room = web.arg("room");
    const String device = web.arg("device"), token = web.arg("token");
    if (!ssid.length() || ssid.length() > 32 || password.length() > 64 ||
        (password.length() && password.length() < 8) ||
        !safeCode(room, 32) || !validUuid(device) ||
        token.length() != 64 || !safeCode(token, 64, true)) {
        web.send(400, "text/plain", "Check the Wi-Fi name, room code, device UUID and token.");
        return;
    }
    Settings next{};
    ssid.toCharArray(next.ssid, sizeof(next.ssid));
    password.toCharArray(next.password, sizeof(next.password));
    room.toCharArray(next.room, sizeof(next.room));
    device.toCharArray(next.device, sizeof(next.device));
    String normalizedToken = token;
    normalizedToken.toLowerCase();
    normalizedToken.toCharArray(next.token, sizeof(next.token));
    if (preferences.putBytes("settings", &next, sizeof(next)) != sizeof(next)) {
        web.send(500, "text/plain", "Could not save settings.");
        return;
    }
    // Uploader receives a consistent settings snapshot for every request.
    xQueueOverwrite(uploadConfig, &next);
    config = next;
    submitted = true;
    web.send(200, "text/plain", "Saved. Check USB Serial for the connection result. Reload /setup if needed.");
    WiFi.begin(config.ssid, config.password);
    attemptAt = millis();
}

void dashboard() {
    if (setupClient()) {
        setupPage();
        return;
    }
    web.send(200, "text/html", R"HTML(<!doctype html><meta name="viewport" content="width=device-width">
<title>Smart Classroom</title><h1>Classroom status</h1><pre id="status"></pre>
<p><a href="/setup">Wi-Fi setup (setup network only)</a></p>
<script>async function poll(){try{const r=await fetch('/status',{cache:'no-store'});
document.querySelector('#status').textContent=(await r.text());}
catch{document.querySelector('#status').textContent='Device unreachable';}
setTimeout(poll,1000)}poll()</script>)HTML");
}

void statusPage() {
    const auto s = classroomReading();
    const int lastRole = role.load();
    String status = String("Room: ") + config.room + "\nDevice: " + config.device +
        "\nWi-Fi: " + (wifiState.load() ? "connected" : "offline") + "\nIP: " +
        (wifiState.load() ? WiFi.localIP() : WiFi.softAPIP()).toString() +
        "\nUptime (s): " + String(millis()/1000) + "\nTemperature (C): " +
        (s.dhtOk ? String(s.temperature10/10.0f,1) : "NA") + "\nHumidity (%): " +
        (s.dhtOk ? String(s.humidity10/10.0f,1) : "NA") + "\nDHT11: " + (s.dhtOk ? "OK" : "INVALID") +
        "\nFlame: " + (s.flame ? "DETECTED" : "CLEAR") + "\nRFID: " + (s.rfidOk ? "OK" : "INVALID") +
        "\nLast card: " + (lastRole == 1 ? "TEACHER" : lastRole == 2 ? "STUDENT" : lastRole ? "UNKNOWN" : "NONE") +
        "\nLast scan (uptime ms): " + String(scanAt.load()) + "\nScans: " + String(s.cards) +
        "\nWatchdog: " + (s.watchdog ? "ACTIVE" : "ERROR");
    const auto safety = classroomSafety();
    status += String("\nDoor (software): ") + safety.door + "\nSafety: " + safety.mode +
        "\nHeat index (C): " + (safety.valid ? String(safety.heat10/10.0f,1) : "INVALID") +
        "\nHeat trend: " + safety.trend + "\nLast cloud acknowledgment (uptime ms): " + String(lastUpload.load()) +
        "\nPending: " + String(uxQueueMessagesWaiting(pending) + uxQueueMessagesWaiting(urgentKinds) +
            uxQueueMessagesWaiting(urgentRecords) + (inFlight.load() ? 1 : 0)) +
        "\nDropped: " + String(drops.load()) + "\nFirmware: " + Firmware;
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "text/plain", status);
}

void cardDemo() {
    if (!setupClient()) {
        web.send(404);
        return;
    }
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "text/html", R"HTML(<!doctype html><meta name="viewport" content="width=device-width">
<title>Private card demo</title><h1>Last RFID UID</h1><pre id="card"></pre>
<script>async function poll(){try{const r=await fetch('/demo/uid',{cache:'no-store'});
document.querySelector('#card').textContent=r.ok?await r.text():'Demo unavailable';}
catch{document.querySelector('#card').textContent='Device unreachable';}
setTimeout(poll,750)}poll()</script>)HTML");
}

void cardUid() {
    if (!setupClient()) {
        web.send(404);
        return;
    }
    Card card{};
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "text/plain", xQueuePeek(latestCard, &card, 0) == pdPASS ? card.uid : "No scan yet");
}

void capture(const char* kind, const Urgent* urgent = nullptr) {
    Record record{};
    char event[37], timestamp[32] = "null", temperature[16] = "null", humidity[16] = "null";
    uuid(event);
    const auto s = classroomReading();
    const time_t now = urgent ? urgent->captured : time(nullptr);
    const bool synced = now >= 1700000000;
    if (synced) {
        tm utc;
        gmtime_r(&now, &utc);
        strftime(timestamp, sizeof(timestamp), "\"%Y-%m-%dT%H:%M:%SZ\"", &utc);
    }
    if (s.dhtOk) {
        snprintf(temperature, sizeof(temperature), "%.1f", s.temperature10/10.0f);
        snprintf(humidity, sizeof(humidity), "%.1f", s.humidity10/10.0f);
    }
#ifndef CLASSROOM_LEVEL2_ONLY
    char heat[16] = "null";
    const auto safety = urgent ? urgent->safety : classroomSafety();
    if (safety.valid) snprintf(heat, sizeof(heat), "%.1f", safety.heat10/10.0f);
#endif
    snprintf(record.body, sizeof(record.body),
             "{\"event_id\":\"%s\",\"boot_id\":\"%s\",\"sequence_no\":%u,\"kind\":\"%s\","
             "\"captured_at\":%s,\"clock_quality\":\"%s\",\"temperature_c\":%s,\"humidity_pct\":%s,"
             "\"dht_ok\":%s,\"rfid_ok\":%s,\"flame\":%s,\"watchdog_ok\":%s,\"uptime_ms\":%lu,"
             "\"wifi_rssi_dbm\":%ld,\"free_heap_bytes\":%lu,\"reset_reason\":%d,\"firmware\":\"%s\""
#ifndef CLASSROOM_LEVEL2_ONLY
             ",\"heat_index_c\":%s,\"safety\":\"%s\",\"door\":\"%s\",\"fire_latched\":%s,\"panic_latched\":%s"
#endif
             "}",
             event, boot, sequence++, kind, timestamp, synced ? "synced" : "unknown", temperature, humidity,
             s.dhtOk ? "true" : "false", s.rfidOk ? "true" : "false", s.flame ? "true" : "false",
             s.watchdog ? "true" : "false", static_cast<unsigned long>(urgent ? urgent->uptime : millis()),
             static_cast<long>(wifiState.load() ? constrain(WiFi.RSSI(), -127, 0) : -127),
             static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<int>(esp_reset_reason()), Firmware
#ifndef CLASSROOM_LEVEL2_ONLY
             , heat, safety.mode, safety.door, safety.fire ? "true" : "false", safety.panic ? "true" : "false"
#endif
             );
    const bool routine = strcmp(kind,"telemetry") == 0;
    const auto queue = routine ? pending : urgentRecords;
    if (xQueueSend(queue, &record, 0) != pdPASS) {
        Record discarded;
        xQueueReceive(queue, &discarded, 0);
        xQueueSend(queue, &record, 0);
        ++drops;
    }
}

void uploadTask(void*) {
    Record current{}, last{};
    uint32_t retryAt = 0, backoff = 1000;
    bool waiting = false, urgent = false;
    for (;;) {
        Settings settings{};
        if (uxQueueMessagesWaiting(urgentRecords) && !urgent) {
            if (waiting && xQueueSendToFront(pending, &current, 0) != pdPASS) ++drops;
            waiting = xQueueReceive(urgentRecords, &current, 0) == pdPASS;
            urgent = waiting;
            retryAt = 0;
        }
        if (wifiState.load() && xQueuePeek(uploadConfig, &settings, 0) == pdPASS &&
            settings.token[0] && time(nullptr) >= 1700000000 &&
            (retryAt == 0 || static_cast<int32_t>(millis() - retryAt) >= 0)) {
            if (!waiting && resendRequested.exchange(false) && last.body[0]) {
                current = last;
                waiting = true;
            }
            if (!waiting) waiting = xQueueReceive(pending, &current, 0) == pdPASS;
            inFlight.store(waiting);
            if (waiting) {
                NetworkClientSecure tls;
                tls.useBuiltinCACertBundle();
                tls.setHandshakeTimeout(3);
                HTTPClient http;
                http.setConnectTimeout(2500);
                http.setTimeout(2500);
                int code = -1;
                if (http.begin(tls, Endpoint)) {
                    http.addHeader("Content-Type", "application/json");
                    http.addHeader("X-Device-Token", settings.token);
                    http.addHeader("X-Device-ID", settings.device);
                    http.addHeader("X-Room-Code", settings.room);
                    code = http.POST(current.body);
                    http.end();
                }
                cloudCode.store(code);
                if (code == 200 || code == 201) {
                    lastUpload.store(millis());
                    last = current;
                    waiting = false;
                    urgent = false;
                    inFlight.store(false);
                    backoff = 1000;
                    retryAt = millis() + 400;
                    Serial.printf("CLOUD ACK: %s\n", code == 200 ? "DUPLICATE" : "ACCEPTED");
                } else {
                    if (code == 400 || code == 409 || code == 413) {
                        waiting = false;
                        urgent = false;
                        inFlight.store(false);
                        ++drops;
                    }
                    retryAt = millis() + backoff;
                    backoff = std::min<uint32_t>(backoff * 2U, 8000U);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void networkTask(void*) {
    preferences.begin("classroom", false);
    preferences.getBytes("settings", &config, sizeof(config));
    xQueueOverwrite(uploadConfig, &config);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    snprintf(hostname, sizeof(hostname), "smartclass-%06lx",
             static_cast<unsigned long>(ESP.getEfuseMac() & 0xffffff));
    WiFi.setHostname(hostname);
    if (config.ssid[0]) WiFi.begin(config.ssid, config.password);
    else startSetup();
    attemptAt = millis();
    web.on("/", HTTP_GET, dashboard);
    web.on("/status", HTTP_GET, statusPage);
    web.on("/setup", HTTP_GET, setupPage);
    web.on("/setup", HTTP_POST, saveSetup);
    web.on("/demo/card", HTTP_GET, cardDemo);
    web.on("/demo/uid", HTTP_GET, cardUid);
    const char* headers[] = {"Origin"};
    web.collectHeaders(headers, 1);
    web.begin();
    capture("boot");
    uint32_t sampledAt = millis();
    bool everOnline = false;
    for (;;) {
        const uint32_t now = millis();
        Urgent urgent{};
        for (unsigned i = 0; i < 16 && xQueueReceive(urgentKinds, &urgent, 0) == pdPASS; ++i)
            capture(urgent.kind, &urgent);
        const bool connected = WiFi.status() == WL_CONNECTED;
        const bool wasConnected = wifiState.exchange(connected);
        if (connected && !wasConnected) {
            everOnline = true;
            configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
            MDNS.begin(hostname);
            MDNS.addService("http", "tcp", 80);
            Serial.printf("WIFI CONNECTED: %s http://%s.local cloud=waiting\n",
                          WiFi.localIP().toString().c_str(), hostname);
        } else if (!connected && wasConnected) {
            attemptAt = now;
            MDNS.end();
            Serial.println("WIFI OFFLINE: buffering telemetry");
        }
        if (connected && provisioning && submitted && WiFi.SSID() == config.ssid &&
            now - attemptAt >= 1000) {
            WiFi.softAPdisconnect(true);
            provisioning = submitted = false;
        }
        if (setupRequested.exchange(false)) {
            submitted = false;
            startSetup();
        }
        if (!connected && config.ssid[0] && now - attemptAt >= 10000) {
            if (!everOnline || submitted) startSetup();
            WiFi.begin(config.ssid, config.password);
            attemptAt = now;
        }
        if (now - sampledAt >= 5000) {
            sampledAt = now;
            capture("telemetry");
        }
        web.handleClient();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}

void classroomNetworkBegin() {
    uuid(boot);
    pending = xQueueCreate(Capacity, sizeof(Record));
    uploadConfig = xQueueCreate(1, sizeof(Settings));
    latestCard = xQueueCreate(1, sizeof(Card));
    urgentKinds = xQueueCreate(16, sizeof(Urgent));
    urgentRecords = xQueueCreate(16, sizeof(Record));
    if (!pending || !uploadConfig || !latestCard || !urgentKinds || !urgentRecords ||
        xTaskCreate(uploadTask, "CloudUpload", 12288, nullptr, 1, nullptr) != pdPASS ||
        xTaskCreate(networkTask, "ClassroomWeb", 8192, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("NETWORK ERROR: initialization failed");
    }
}

void classroomUrgent(const char* kind) {
    const Urgent event{kind, classroomSafety(), time(nullptr), millis()};
    if (urgentKinds && xQueueSend(urgentKinds, &event, 0) != pdPASS) ++drops;
}

void classroomCard(const char* cardRole, const char* uid) {
    if (latestCard) {
        Card card{};
        strlcpy(card.uid, uid, sizeof(card.uid));
        xQueueOverwrite(latestCard, &card);
    }
    scanAt.store(millis());
    role.store(strcmp(cardRole,"TEACHER") == 0 ? 1 : strcmp(cardRole,"STUDENT") == 0 ? 2 : 3);
}

bool classroomNetworkCommand(const String& command) {
    if (command == "wifi setup") setupRequested.store(true);
    else if (command == "wifi status") Serial.printf("WIFI: %s\n", wifiState.load() ? "CONNECTED" : "OFFLINE");
    else if (command == "cloud status") Serial.printf("CLOUD: code=%d ack_uptime_ms=%u\n", cloudCode.load(), lastUpload.load());
    else if (command == "queue status") Serial.printf("QUEUE: pending=%u dropped=%u\n",
        pending && urgentKinds && urgentRecords ? static_cast<unsigned>(uxQueueMessagesWaiting(pending) +
            uxQueueMessagesWaiting(urgentKinds) + uxQueueMessagesWaiting(urgentRecords)) + (inFlight.load() ? 1 : 0) : 0,
        drops.load());
    else if (command == "test resend") { resendRequested.store(true); Serial.println("CLOUD: resend requested"); }
    else return false;
    return true;
}
