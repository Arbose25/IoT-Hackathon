
#include <Arduino.h>
#include "classroom_network.h"
#include "classroom_safety.h"
#include <SPI.h>
#include <MFRC522.h>
#include "dht11_capture.h"
#include <driver/gpio.h>
#include <esp_task_wdt.h>
#include <freertos/queue.h>

#if __has_include("level1.local.h")
#include "level1.local.h"
#endif

#ifndef LEVEL1_TEACHER_UID
#define LEVEL1_TEACHER_UID ""
#endif
#ifndef LEVEL1_STUDENT_UID
#define LEVEL1_STUDENT_UID ""
#endif
#ifndef LEVEL1_BUZZER_MODE
#define LEVEL1_BUZZER_MODE 0
#endif
#ifndef LEVEL1_BUZZER_ACTIVE_LOW
#define LEVEL1_BUZZER_ACTIVE_LOW 0
#endif
#include <atomic>
#include <math.h>
#include <string.h>
#include <strings.h>

#ifdef CLASSROOM_LEVEL1_ONLY
void classroomSafetySample(float, float) {}
void classroomCard(const char*, const char*) {}
void classroomSafetyCard(bool) {}
ClassroomSafety classroomSafety() { return {"MONITORING", "UNLOCKED", "", 0, false, false, false}; }
bool classroomSafetyLed(uint32_t, uint8_t&, uint8_t&, uint8_t&) { return false; }
bool classroomSafetyTone(uint32_t, bool&) { return false; }
bool classroomNetworkCommand(const String&) { return false; }
bool classroomSafetyCommand(const String&) { return false; }
void classroomSafetyBegin() {}
void classroomNetworkBegin() {}
void classroomSafetyTick(bool) {}
#endif

// ==============================================
// SMART CLASSROOM - LEVEL 1
// ESP32-S3-N16R8
//
// DHT11 + RC522 + Flame + FreeRTOS
// Serial Commands + 2Hz Heartbeat
//
// Local monitoring and demonstration card identification.
// ==============================================

// ==============================================
// 1. PIN CONFIGURATION
// ==============================================

#define DHT_PIN 1

#define FLAME_PIN 2
#define FLAME_ACTIVE_LEVEL LOW

#define RFID_SS_PIN   41
#define RFID_SCK_PIN  39
#define RFID_MOSI_PIN 40
#define RFID_MISO_PIN 38
#define RFID_RST_PIN  42

constexpr uint8_t HEARTBEAT_LED_PIN = 48;
constexpr uint8_t BUZZER_PIN = 47;
constexpr uint8_t assignedPins[] = {
    DHT_PIN, FLAME_PIN, RFID_SS_PIN, RFID_SCK_PIN, RFID_MOSI_PIN,
    RFID_MISO_PIN, RFID_RST_PIN, HEARTBEAT_LED_PIN, BUZZER_PIN
};

constexpr bool pinsAreUnique() {
    for (size_t i = 0; i < sizeof(assignedPins); ++i) {
        for (size_t j = i + 1; j < sizeof(assignedPins); ++j) {
            if (assignedPins[i] == assignedPins[j]) {
                return false;
            }
        }
    }
    return true;
}

static_assert(pinsAreUnique(), "Level 1 GPIO assignments overlap");
static_assert(LEVEL1_BUZZER_MODE >= 0 && LEVEL1_BUZZER_MODE <= 2,
              "Buzzer mode must be 0 (off), 1 (active), or 2 (passive)");

// ==============================================
// 2. POLLING INTERVALS
// ==============================================

constexpr uint32_t DHT_INTERVAL = 5000;
constexpr uint32_t RFID_INTERVAL = 200;
constexpr uint32_t FLAME_INTERVAL = 100;
constexpr uint32_t HEALTH_INTERVAL = 1000;

// 250ms ON + 250ms OFF = 2Hz
constexpr uint32_t HEARTBEAT_INTERVAL = 250;

// ==============================================
// 3. SENSOR OBJECTS
// ==============================================

Dht11Capture dht(static_cast<gpio_num_t>(DHT_PIN));
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

// ==============================================
// 4. THREAD-SAFE VARIABLES
// ==============================================

// Task cycle counters
std::atomic<uint32_t> sensorCycles{0};
std::atomic<uint32_t> rfidCycles{0};
std::atomic<uint32_t> flameCycles{0};
std::atomic<uint32_t> healthCycles{0};

// CPU core tracking
std::atomic<int> sensorCore{-1};
std::atomic<int> rfidCore{-1};
std::atomic<int> flameCore{-1};
std::atomic<int> healthCore{-1};

// 0 = STARTING, 1 = OK, 2 = ERROR
std::atomic<int> dhtState{0};
std::atomic<int> rfidState{0};

// DHT11 values, multiplied by 10
std::atomic<int32_t> temperature10{0};
std::atomic<int32_t> humidity10{0};

// Flame monitoring
std::atomic<bool> flameDetected{false};
std::atomic<int> flameRaw{HIGH};

// RFID events
std::atomic<uint32_t> cardCount{0};

// Heartbeat state
uint32_t lastHeartbeat = 0;
uint32_t heartbeatTransitions = 0;
bool heartbeatOn = false;

constexpr uint32_t WATCHDOG_TIMEOUT_MS = 15000;
constexpr unsigned WATCHED_TASKS = 4;
bool watchdogReady = false;
std::atomic<unsigned> watchdogSubscribed{0};
std::atomic<bool> watchdogError{false};

enum FaultMask : uint8_t { SIM_DHT = 1, SIM_RFID = 2 };
std::atomic<uint8_t> simulatedFaults{0};

QueueHandle_t buzzerQueue = nullptr;
bool buzzerReady = false;
std::atomic<unsigned> buzzerDropped{0};

// Worker tasks enqueue complete lines. The Arduino loop owns Serial output.
struct LogLine { char text[256]; };
QueueHandle_t taskLogs;

template<typename... Args>
void taskLog(const char* format, Args... args) {
    if (!taskLogs) return;
    LogLine line{};
    snprintf(line.text, sizeof(line.text), format, args...);
    xQueueSend(taskLogs, &line, 0);
}

void configureWatchdog() {
    esp_task_wdt_config_t config{};
    config.timeout_ms = WATCHDOG_TIMEOUT_MS;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    config.idle_core_mask |= 1U << 0;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    config.idle_core_mask |= 1U << 1;
#endif
    // Report timeouts without deliberately restarting the demonstration board.
    config.trigger_panic = false;
    esp_err_t result = esp_task_wdt_reconfigure(&config);
    if (result == ESP_ERR_INVALID_STATE) {
        result = esp_task_wdt_init(&config);
    }
    watchdogReady = result == ESP_OK;
    if (!watchdogReady) {
        Serial.printf("WATCHDOG CONFIG ERROR: %s\n", esp_err_to_name(result));
    }
}

bool subscribeWatchdog() {
    if (!watchdogReady) {
        return false;
    }
    const esp_err_t result = esp_task_wdt_add(nullptr);
    if (result != ESP_OK || esp_task_wdt_status(nullptr) != ESP_OK) {
        watchdogError.store(true);
        taskLog("WATCHDOG SUBSCRIPTION ERROR: %s\n", pcTaskGetName(nullptr));
        return false;
    }
    watchdogSubscribed.fetch_add(1);
    return true;
}

void feedWatchdog(bool subscribed) {
    if (subscribed && esp_task_wdt_reset() != ESP_OK &&
        !watchdogError.exchange(true)) {
        taskLog("WATCHDOG FEED ERROR: %s\n", pcTaskGetName(nullptr));
    }
}

const char* watchdogName() {
    if (!watchdogReady || watchdogError.load()) {
        return "ERROR";
    }
    return watchdogSubscribed.load() == WATCHED_TASKS ? "ACTIVE" : "PENDING";
}

ClassroomReading classroomReading() {
    const auto simulated = simulatedFaults.load();
    return {temperature10.load(), humidity10.load(),
            dhtState.load() == 1 && !(simulated & SIM_DHT),
            rfidState.load() == 1 && !(simulated & SIM_RFID),
            flameDetected.load(), strcmp(watchdogName(), "ACTIVE") == 0,
            cardCount.load()};
}

// ==============================================
// 5. STATUS HELPERS
// ==============================================

const char* stateName(int state) {
    switch (state) {
        case 1: return "OK";
        case 2: return "ERROR";
        default: return "STARTING";
    }
}

const char* effectiveState(int realState, uint8_t mask, FaultMask bit) {
    if (realState == 2) {
        return "ERROR";
    }
    return (mask & bit) != 0 ? "SIM_ERROR" : stateName(realState);
}

const char* flameName() {
    return flameDetected.load()
        ? "DETECTED"
        : "CLEAR";
}

// ==============================================
// 6. DHT11 TASK - CORE 0
// ==============================================

void sensorTask(void* parameter) {
    sensorCore.store(xPortGetCoreID());
    const bool watched = subscribeWatchdog();

    // Sensor stabilization time
    vTaskDelay(pdMS_TO_TICKS(2000));

    TickType_t lastWake = xTaskGetTickCount();
    uint8_t failures = 0;
    constexpr uint8_t recoverySamples = 2;
    uint8_t validStreak = 0;

    for (;;) {
        sensorCycles.fetch_add(1);

        float humidity = NAN;
        float temperature = NAN;
        const bool valid = dht.read(temperature, humidity);

        if (!valid) {
            validStreak = 0;
            classroomSafetySample(NAN, NAN);
            if (failures < 255) {
                failures++;
            }

            // Detect persistent failure
            if (failures >= 2) {
                int previous = dhtState.exchange(2);

                if (previous != 2) {
                    taskLog("SENSOR ERROR: DHT11\n");
                }
            }
        }
        else if (dhtState.load() == 2 && ++validStreak < recoverySamples) {
            taskLog("DHT11 RECOVERY CHECK: %u/%u valid samples\n",
                    static_cast<unsigned>(validStreak), static_cast<unsigned>(recoverySamples));
        }
        else {
            failures = 0;
            validStreak = 0;
            classroomSafetySample(temperature, humidity);

            temperature10.store(
                (int32_t)lroundf(temperature * 10.0f)
            );

            humidity10.store(
                (int32_t)lroundf(humidity * 10.0f)
            );

            int previous = dhtState.exchange(1);

            if (previous == 2) {
                taskLog("DHT11 RECOVERED\n");
            }

            taskLog(
                "DHT11 READ temp=%.1fC humidity=%.1f%%\n",
                temperature,
                humidity
            );
        }

        feedWatchdog(watched);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DHT_INTERVAL));
    }
}

// ==============================================
// 7. RFID TASK - CORE 1
// ==============================================

bool readerResponds() {
    const byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    return version != 0x00 && version != 0xFF;
}

bool readerConfigured() {
    return rfid.PCD_ReadRegister(MFRC522::TReloadRegL) == 0xE8 &&
           (rfid.PCD_ReadRegister(MFRC522::TxControlReg) & 0x03) == 0x03;
}

void reportCard(const char* uid) {
    cardCount.fetch_add(1);
    uint8_t beeps = 3;
    if (strcasecmp(uid, LEVEL1_TEACHER_UID) == 0) {
        beeps = 1;
        taskLog("CARD role=TEACHER uid=%s\n", uid);
    } else if (strcasecmp(uid, LEVEL1_STUDENT_UID) == 0) {
        beeps = 2;
        taskLog("CARD role=STUDENT uid=%s\n", uid);
    } else {
        taskLog("CARD role=UNKNOWN\n");
    }
    classroomCard(beeps == 1 ? "TEACHER" : beeps == 2 ? "STUDENT" : "UNKNOWN", uid);
    classroomSafetyCard(beeps == 1);
    if (buzzerReady && xQueueSend(buzzerQueue, &beeps, 0) != pdPASS) {
        buzzerDropped.fetch_add(1);
    }
}

void rfidTask(void* parameter) {
    rfidCore.store(xPortGetCoreID());
    const bool watched = subscribeWatchdog();
    TickType_t lastWake = xTaskGetTickCount();
    uint8_t failures = 0;
    uint8_t missedPolls = 0;
    bool needsInit = true;
    char previousUid[21] = {}; // ISO 14443A UIDs are up to 10 bytes.

    for (;;) {
        rfidCycles.fetch_add(1);
        const bool responding = readerResponds();
        if (responding && (needsInit || !readerConfigured())) {
            rfid.PCD_Init();
            needsInit = !readerResponds() || !readerConfigured();
        }

        if (!responding || needsInit) {
            needsInit = true;
            // Communication loss is ambiguous. Keep the last presentation latched.
            missedPolls = 0;
            if (failures < 3 && ++failures == 3) {
                rfidState.store(2);
                taskLog("SENSOR ERROR: RC522\n");
            }
        } else {
            failures = 0;
            if (rfidState.exchange(1) == 2) {
                taskLog("RC522 RECOVERED\n");
            }

            byte atqa[2];
            byte atqaSize = sizeof(atqa);
            const auto result = rfid.PICC_WakeupA(atqa, &atqaSize);
            // Only a reader-generated RF timeout counts as a clear field.
            const bool noCard = result == MFRC522::STATUS_TIMEOUT &&
                                readerResponds() && readerConfigured() &&
                                (rfid.PCD_ReadRegister(MFRC522::ComIrqReg) & 0x01) != 0;
            if (noCard) {
                if (missedPolls < 2 && ++missedPolls == 2) {
                    previousUid[0] = '\0';
                }
            } else {
                // A collision or failed selection is not evidence of card removal.
                missedPolls = 0;
                if ((result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION) &&
                    rfid.PICC_ReadCardSerial() && rfid.uid.size > 0 && rfid.uid.size <= 10) {
                    char uid[sizeof(previousUid)] = {};
                    for (byte i = 0; i < rfid.uid.size; ++i) {
                        snprintf(uid + 2 * i, 3, "%02X",
                                 static_cast<unsigned>(rfid.uid.uidByte[i]));
                    }
                    rfid.PICC_HaltA();
                    rfid.PCD_StopCrypto1();
                    if (strcmp(uid, previousUid) != 0) {
                        strcpy(previousUid, uid);
                        reportCard(uid);
                    }
                }
            }
        }

        feedWatchdog(watched);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(RFID_INTERVAL));
    }
}

// ==============================================
// 8. FLAME SENSOR TASK - CORE 0
// ==============================================

void flameTask(void* parameter) {
    flameCore.store(xPortGetCoreID());
    const bool watched = subscribeWatchdog();

    TickType_t lastWake = xTaskGetTickCount();
    bool firstReading = true;

    for (;;) {
        flameCycles.fetch_add(1);

        int raw = digitalRead(FLAME_PIN);

        bool detected =
            (raw == FLAME_ACTIVE_LEVEL);

        flameRaw.store(raw);

        bool previous = flameDetected.exchange(
            detected
        );

        if (firstReading || detected != previous) {
            taskLog(
                "FLAME raw=%d state=%s\n",
                raw,
                detected ? "DETECTED" : "CLEAR"
            );

            firstReading = false;
        }

        feedWatchdog(watched);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(FLAME_INTERVAL));
    }
}

// Health task owns the one-second status schedule.
void healthTask(void* parameter) {
    healthCore.store(xPortGetCoreID());
    const bool watched = subscribeWatchdog();
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(HEALTH_INTERVAL));
        healthCycles.fetch_add(1);
        const auto mask = simulatedFaults.load();
        const int dhtHealth = dhtState.load();
        const int rfidHealth = rfidState.load();
        char temp[16] = "NA";
        char humidity[16] = "NA";
        if (dhtHealth == 1 && !(mask & SIM_DHT)) {
            snprintf(temp, sizeof(temp), "%.1f", temperature10.load() / 10.0f);
            snprintf(humidity, sizeof(humidity), "%.1f", humidity10.load() / 10.0f);
        }
        taskLog(
            "STATUS uptime=%lu dht=%s temp=%s humidity=%s rfid=%s flame=%s "
            "cards=%lu watchdog=%s door=%s safety=%s sim_dht=%u sim_rfid=%u\n",
            static_cast<unsigned long>(millis() / 1000),
            effectiveState(dhtHealth, mask, SIM_DHT), temp, humidity,
            effectiveState(rfidHealth, mask, SIM_RFID), flameName(),
            static_cast<unsigned long>(cardCount.load()), watchdogName(),
            classroomSafety().door, classroomSafety().mode,
            static_cast<unsigned>((mask & SIM_DHT) != 0),
            static_cast<unsigned>((mask & SIM_RFID) != 0));
        feedWatchdog(watched);
    }
}

void updateHeartbeat() {
    static int previousColor = -1;
    const uint32_t now = millis();
    uint8_t r, g, b;
    if (classroomSafetyLed(now, r, g, b)) {
        const int color = 256 + (r << 16) + (g << 8) + b;
        if (previousColor != color) rgbLedWrite(HEARTBEAT_LED_PIN, r, g, b);
        previousColor = color;
        return;
    }
    if (previousColor >= 256) previousColor = -1;
    const bool fault = dhtState.load() == 2 || rfidState.load() == 2 ||
                       simulatedFaults.load() != 0;
    if (fault) {
        if (previousColor != 2) {
            rgbLedWrite(HEARTBEAT_LED_PIN, 16, 0, 16);
            previousColor = 2;
        }
        return;
    }

    if (previousColor < 0 || previousColor == 2) {
        lastHeartbeat = now;
        heartbeatOn = true;
    } else if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        const auto periods = (now - lastHeartbeat) / HEARTBEAT_INTERVAL;
        lastHeartbeat += periods * HEARTBEAT_INTERVAL;
        heartbeatTransitions += periods;
        if (periods % 2 != 0) {
            heartbeatOn = !heartbeatOn;
        }
    }
    if (previousColor != static_cast<int>(heartbeatOn)) {
        rgbLedWrite(HEARTBEAT_LED_PIN, 0, heartbeatOn ? 16 : 0, 0);
        previousColor = heartbeatOn;
    }
}

void setBuzzer(bool on) {
    if (LEVEL1_BUZZER_MODE == 1) {
        digitalWrite(BUZZER_PIN, on != (LEVEL1_BUZZER_ACTIVE_LOW != 0));
    } else if (LEVEL1_BUZZER_MODE == 2) {
        ledcWrite(BUZZER_PIN, on ? 128 : 0);
    }
}

void configureBuzzer() {
    if (LEVEL1_BUZZER_MODE == 0) {
        return; // Leave the pin untouched until the hardware is confirmed.
    }
    buzzerQueue = xQueueCreate(8, sizeof(uint8_t));
    if (buzzerQueue == nullptr) {
        Serial.println("BUZZER ERROR: queue allocation failed");
        return;
    }
    if (LEVEL1_BUZZER_MODE == 1) {
        pinMode(BUZZER_PIN, OUTPUT);
    } else if (!ledcAttach(BUZZER_PIN, 2400, 8)) {
        Serial.println("BUZZER ERROR: PWM initialization failed");
        return;
    }
    setBuzzer(false);
    buzzerReady = true;
}

void updateBuzzer() {
    if (!buzzerReady) {
        return;
    }
    constexpr uint32_t beepMs = 90;
    constexpr uint32_t gapMs = 100;
    static uint32_t changedAt = 0;
    static uint8_t remaining = 0;
    static bool sounding = false;
    static bool alerting = false;
    const uint32_t now = millis();
    bool alertOn;
    if (classroomSafetyTone(now, alertOn)) {
        setBuzzer(alertOn);
        alerting = true;
        return;
    }
    if (alerting) {
        setBuzzer(false);
        alerting = sounding = false;
        changedAt = now;
    }

    if (sounding) {
        if (now - changedAt >= beepMs) {
            setBuzzer(false);
            sounding = false;
            changedAt = now;
        }
        return;
    }
    if (now - changedAt < gapMs) {
        return;
    }
    if (remaining == 0 && xQueueReceive(buzzerQueue, &remaining, 0) != pdPASS) {
        return;
    }
    --remaining;
    setBuzzer(true);
    sounding = true;
    changedAt = now;
}

// ==============================================
// 11. PRINT TASK INFORMATION
// ==============================================

void printTasks() {
    Serial.println();
    Serial.println("===== ACTIVE TASKS =====");

    Serial.printf(
        "SensorTask: Core %d, cycles=%lu\n",
        sensorCore.load(),
        (unsigned long)sensorCycles.load()
    );

    Serial.printf(
        "RFIDTask: Core %d, cycles=%lu\n",
        rfidCore.load(),
        (unsigned long)rfidCycles.load()
    );

    Serial.printf(
        "FlameTask: Core %d, cycles=%lu\n",
        flameCore.load(),
        (unsigned long)flameCycles.load()
    );

    Serial.printf(
        "HealthTask: Core %d, cycles=%lu\n",
        healthCore.load(),
        (unsigned long)healthCycles.load()
    );
}

// ==============================================
// 12. PRINT SYSTEM HEALTH
// ==============================================

void printHealth() {
    Serial.println();
    Serial.println("===== SYSTEM HEALTH =====");

    Serial.printf(
        "UPTIME: %lu seconds\n",
        (unsigned long)(millis() / 1000)
    );

    Serial.printf(
        "FREE HEAP: %lu bytes\n",
        (unsigned long)ESP.getFreeHeap()
    );

    Serial.printf(
        "CPU CORES: %d\n",
        portNUM_PROCESSORS
    );

    const auto mask = simulatedFaults.load();
    const auto dhtHealth = dhtState.load();
    Serial.printf("DHT11: %s (real=%s)\n",
                  effectiveState(dhtHealth, mask, SIM_DHT), stateName(dhtHealth));

    if (dhtHealth == 1 && !(mask & SIM_DHT)) {
        Serial.printf(
            "TEMPERATURE: %.1f C\n",
            temperature10.load() / 10.0f
        );

        Serial.printf(
            "HUMIDITY: %.1f %%\n",
            humidity10.load() / 10.0f
        );
    }

    const auto rfidHealth = rfidState.load();
    Serial.printf("RFID: %s (real=%s)\n",
                  effectiveState(rfidHealth, mask, SIM_RFID), stateName(rfidHealth));

    Serial.printf(
        "RFID CARD COUNT: %lu\n",
        (unsigned long)cardCount.load()
    );

    Serial.printf(
        "FLAME: %s (raw=%d)\n",
        flameName(),
        flameRaw.load()
    );

    Serial.printf("LED: %s, heartbeat transitions=%lu\n",
                  dhtHealth == 2 || rfidHealth == 2 || mask ? "MAGENTA" : "GREEN HEARTBEAT",
                  static_cast<unsigned long>(heartbeatTransitions));
    Serial.printf("WATCHDOG: %s timeout_ms=%lu tasks=%u/%u\n",
                  watchdogName(), static_cast<unsigned long>(WATCHDOG_TIMEOUT_MS),
                  watchdogSubscribed.load(), WATCHED_TASKS);
    Serial.printf("BUZZER: %s dropped=%u\n",
                  buzzerReady ? (LEVEL1_BUZZER_MODE == 1 ? "ACTIVE" : "PASSIVE") : "DISABLED",
                  buzzerDropped.load());

    Serial.println(
        String("DOOR: ") + classroomSafety().door + " (SOFTWARE STATE ONLY)"
    );

    Serial.println("=========================");
}

// ==============================================
// 13. SERIAL COMMANDS
// ==============================================

void processCommand(const char* input) {
    String command(input);

    command.trim();
    command.toLowerCase();

    if (command == "help") {
        Serial.println("AVAILABLE COMMANDS:");
        Serial.println("help");
        Serial.println("health");
        Serial.println("tasks");
        Serial.println("test dht");
        Serial.println("test rfid");
        Serial.println("test clear");
        Serial.println("test buzzer");
#ifndef CLASSROOM_LEVEL1_ONLY
        Serial.println("wifi status | wifi setup | cloud status | queue status | test resend");
#ifndef CLASSROOM_LEVEL2_ONLY
        Serial.println("safety status | test heat warning/danger/cool/invalid | test panic | test panic pulse");
        Serial.println("test fire | test fire pulse | test fire clear | test safety clear");
#endif
#endif
    }
    else if (command == "health") {
        printHealth();
    }
    else if (command == "tasks") {
        printTasks();
    }
    else if (command == "test dht") {
        simulatedFaults.fetch_or(SIM_DHT);
        Serial.println("SIMULATED SENSOR ERROR: DHT11");
    }
    else if (command == "test rfid") {
        simulatedFaults.fetch_or(SIM_RFID);
        Serial.println("SIMULATED SENSOR ERROR: RC522");
    }
    else if (command == "test clear") {
        simulatedFaults.store(0);
        Serial.println("SIMULATED FAULTS CLEARED");
    }
    else if (command == "test buzzer") {
        const uint8_t beeps = 1;
        if (!buzzerReady) {
            Serial.println(LEVEL1_BUZZER_MODE == 0
                ? "BUZZER TEST: DISABLED"
                : "BUZZER TEST: INITIALIZATION FAILED");
        } else if (xQueueSend(buzzerQueue, &beeps, 0) == pdPASS) {
            Serial.println("BUZZER TEST: ONE BEEP QUEUED");
        } else {
            buzzerDropped.fetch_add(1);
            Serial.println("BUZZER TEST: QUEUE FULL, TRY AGAIN");
        }
    }
    else if (!classroomNetworkCommand(command) && !classroomSafetyCommand(command) && command.length() > 0) {
        Serial.println("UNKNOWN COMMAND");
    }
}

// ==============================================
// 14. NON-BLOCKING SERIAL READER
// ==============================================

void handleSerialInput() {
    static char buffer[64];
    static size_t position = 0;

    static bool overflow = false;
    for (unsigned read = 0; read < 64 && Serial.available() > 0; ++read) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            if (overflow) {
                overflow = false;
                continue;
            }
            if (position == 0) {
                continue;
            }

            buffer[position] = '\0';

            Serial.printf(
                "COMMAND RECEIVED: %s\n",
                buffer
            );

            processCommand(buffer);
            position = 0;
        }
        else if (overflow) {
            continue;
        }
        else if (position < sizeof(buffer) - 1) {
            buffer[position++] = c;
        }
        else {
            position = 0;

            overflow = true;
            Serial.println(
                "ERROR: COMMAND TOO LONG"
            );
        }
    }
}

// ==============================================
// 15. SETUP
// ==============================================

void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("===========================");
    Serial.println("SMART CLASSROOM - LEVEL 1");
    Serial.println("ESP32-S3-N16R8");
#ifdef CLASSROOM_LEVEL1_ONLY
    Serial.println("LEVEL 1 STABILITY BUILD - RMT DHT11");
#endif
    Serial.println("SENSORS + 2HZ HEARTBEAT");
    Serial.println("===========================");

    // Initialize DHT11
    taskLogs = xQueueCreate(16, sizeof(LogLine));
    if (!taskLogs) Serial.println("LOG ERROR: task queue unavailable");

    // Initialize flame input
    pinMode(FLAME_PIN, INPUT);

    // Initialize custom SPI
    pinMode(RFID_SS_PIN, OUTPUT);
    digitalWrite(RFID_SS_PIN, HIGH);

    SPI.begin(
        RFID_SCK_PIN,
        RFID_MISO_PIN,
        RFID_MOSI_PIN,
        RFID_SS_PIN
    );
    // Give an open MISO wire a defined low level for the register probes.
    const auto pullResult = gpio_set_pull_mode(static_cast<gpio_num_t>(RFID_MISO_PIN),
                                               GPIO_PULLDOWN_ONLY);
    if (pullResult != ESP_OK) {
        Serial.printf("RC522 MISO PULL ERROR: %s\n", esp_err_to_name(pullResult));
    }

    pinMode(RFID_RST_PIN, OUTPUT);
    digitalWrite(RFID_RST_PIN, HIGH);
    configureBuzzer();
    configureWatchdog();
    classroomSafetyBegin();

    // Create FreeRTOS tasks
    BaseType_t a = xTaskCreatePinnedToCore(
        sensorTask,
        "SensorTask",
        4096,
        nullptr,
        1,
        nullptr,
        0
    );

    BaseType_t b = xTaskCreatePinnedToCore(
        rfidTask,
        "RFIDTask",
        6144,
        nullptr,
        1,
        nullptr,
        1
    );

    BaseType_t c = xTaskCreatePinnedToCore(
        flameTask,
        "FlameTask",
        3072,
        nullptr,
        1,
        nullptr,
        0
    );

    BaseType_t d = xTaskCreatePinnedToCore(
        healthTask,
        "HealthTask",
        4096,
        nullptr,
        1,
        nullptr,
        0
    );

    if (a == pdPASS &&
        b == pdPASS &&
        c == pdPASS &&
        d == pdPASS) {

        Serial.println(
            "SUCCESS: 4 FreeRTOS tasks created"
        );
    }
    else {
        watchdogError.store(true);
        Serial.println(
            "ERROR: Task creation failed"
        );
    }

    classroomNetworkBegin();
    Serial.println("Type help for commands.");
}

void loop() {
    LogLine line;
    for (unsigned i = 0; taskLogs && i < 4 && xQueueReceive(taskLogs, &line, 0) == pdPASS; ++i)
        Serial.print(line.text);
    classroomSafetyTick(flameDetected.load());
    updateHeartbeat();
    updateBuzzer();
    handleSerialInput();
    vTaskDelay(pdMS_TO_TICKS(5) > 0 ? pdMS_TO_TICKS(5) : 1);
}
