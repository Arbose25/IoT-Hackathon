
#include <Arduino.h>
#include <atomic>

// ==========================================
// SMART CLASSROOM - LEVEL 1
// ESP32-S3 Dual-Core Task Test
// ==========================================

// Thread-safe task counters
std::atomic<uint32_t> sensorCycles{0};
std::atomic<uint32_t> rfidCycles{0};
std::atomic<uint32_t> healthCycles{0};

// CPU core tracking
std::atomic<int> sensorCore{-1};
std::atomic<int> rfidCore{-1};
std::atomic<int> healthCore{-1};

// ==========================================
// TASK 1: SENSOR TASK - CORE 0
// ==========================================

void sensorTask(void *parameter) {
    for (;;) {
        sensorCore.store(xPortGetCoreID());
        sensorCycles.fetch_add(1);

        // Real DHT11 sensor reading comes later

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ==========================================
// TASK 2: RFID TASK - CORE 1
// ==========================================

void rfidTask(void *parameter) {
    for (;;) {
        rfidCore.store(xPortGetCoreID());
        rfidCycles.fetch_add(1);

        // Real RC522 reading comes later

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ==========================================
// TASK 3: HEALTH TASK - CORE 0
// ==========================================

void healthTask(void *parameter) {
    for (;;) {
        healthCore.store(xPortGetCoreID());
        healthCycles.fetch_add(1);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ==========================================
// PRINT TASK INFORMATION
// ==========================================

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
        "HealthTask: Core %d, cycles=%lu\n",
        healthCore.load(),
        (unsigned long)healthCycles.load()
    );

    Serial.println("========================");
}

// ==========================================
// PRINT SYSTEM HEALTH
// ==========================================

void printHealth() {
    Serial.println();
    Serial.println("===== SYSTEM HEALTH =====");

    Serial.println("SYSTEM: RUNNING");

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

    Serial.println("DHT11: NOT CONFIGURED");
    Serial.println("RFID: NOT CONFIGURED");
    Serial.println("FLAME: NOT CONFIGURED");
    Serial.println("WATCHDOG: NOT YET CONFIGURED");
    Serial.println("DOOR: UNLOCKED (INITIAL STATE ONLY)");

    Serial.println("=========================");
}

// ==========================================
// PROCESS SERIAL COMMAND
// ==========================================

void processCommand(const char *input) {
    String command = String(input);

    command.trim();
    command.toLowerCase();

    if (command.length() == 0) {
        return;
    }

    Serial.println();
    Serial.print("COMMAND RECEIVED: ");
    Serial.println(command);

    if (command == "tasks") {
        printTasks();
    }
    else if (command == "health") {
        printHealth();
    }
    else if (command == "help") {
        Serial.println("AVAILABLE COMMANDS:");
        Serial.println("tasks  - Show FreeRTOS tasks");
        Serial.println("health - Show system health");
        Serial.println("help   - Show available commands");
    }
    else {
        Serial.println("UNKNOWN COMMAND");
        Serial.println("Type 'help' for commands.");
    }
}

// ==========================================
// NON-BLOCKING SERIAL INPUT
// ==========================================

void handleSerialInput() {
    static char buffer[64];
    static size_t position = 0;

    while (Serial.available() > 0) {
        char c = (char)Serial.read();

        // Handle Enter (LF or CRLF)
        if (c == '\n' || c == '\r') {
            if (position > 0) {
                buffer[position] = '\0';
                processCommand(buffer);
                position = 0;
            }
        }
        else if (position < sizeof(buffer) - 1) {
            buffer[position++] = c;
        }
        else {
            position = 0;
            Serial.println("ERROR: COMMAND TOO LONG");
        }
    }
}

// ==========================================
// SETUP
// ==========================================

void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("===========================");
    Serial.println("SMART CLASSROOM SYSTEM");
    Serial.println("ESP32-S3 LEVEL 1");
    Serial.println("SYSTEM STARTING...");
    Serial.println("===========================");

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
        4096,
        nullptr,
        1,
        nullptr,
        1
    );

    BaseType_t c = xTaskCreatePinnedToCore(
        healthTask,
        "HealthTask",
        4096,
        nullptr,
        1,
        nullptr,
        0
    );

    if (a == pdPASS && b == pdPASS && c == pdPASS) {
        Serial.println("SUCCESS: 3 FreeRTOS tasks created");
    }
    else {
        Serial.println("ERROR: Task creation failed");
    }

    Serial.println("Type 'help' to view commands.");
}

// ==========================================
// MAIN LOOP
// ==========================================

void loop() {
    static unsigned long lastStatus = 0;

    unsigned long currentMillis = millis();

    // Print STATUS every second
    if (currentMillis - lastStatus >= 1000) {
        lastStatus = currentMillis;

        Serial.printf(
            "STATUS uptime=%lu "
            "sensor_cycles=%lu "
            "rfid_cycles=%lu "
            "health_cycles=%lu "
            "door=UNLOCKED\n",
            (unsigned long)(currentMillis / 1000),
            (unsigned long)sensorCycles.load(),
            (unsigned long)rfidCycles.load(),
            (unsigned long)healthCycles.load()
        );
    }

    // Read commands without blocking for typing
    handleSerialInput();

    delay(10);
}

