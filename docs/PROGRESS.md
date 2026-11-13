# Smart Classroom IoT Hackathon Progress

## Level 1 - Implemented, final recovery verification pending

Previously verified on the ESP32-S3:

- Compiled the firmware with PlatformIO and uploaded it to the board.
- Confirmed that the FreeRTOS sensor and health tasks run on core 0 and the RFID task runs on core 1.
- Verified the Serial commands `help`, `health`, and `tasks`.

Implemented in the Level 1 firmware:

- Four periodic FreeRTOS tasks across both cores for DHT11, RC522, flame input, and health reporting.
- Demonstration card identification with local UID configuration, card counting, and optional active or passive buzzer patterns.
- Green RGB heartbeat, steady magenta fault indication, and automatic DHT11 and RC522 recovery.
- A 15-second task watchdog, once-per-second status, and Serial commands to simulate and clear reported sensor faults.

The operator built and uploaded the RMT-based stability firmware. A Serial log reached 229 seconds without the previous CPU 0 interrupt watchdog panic. The latest decoder rejects invalid DHT11 frames and requires two consecutive valid readings before recovery. The updated recovery sequence and five-minute continuous run are pending final verification. See the [Level 1 hardware checklist](LEVEL1_TEST_GUIDE.md).

## Level 2 - Built, uploaded and connected to Wi-Fi

- Added provisioning, local status, a protected card demo, HTTPS upload and a bounded RAM queue.
- Deployed the device-scoped ingestion function and tested atomic deduplication and credential isolation in a rolled-back transaction.
- The operator reports a successful build, upload and ESP32 Wi-Fi connection. Live ESP32-to-Supabase telemetry and outage tests are pending. See the [Level 2 checklist](LEVEL2_TEST_GUIDE.md).

## Level 3 - Compiled, board tests pending

- Added local heat classification, simulated panic, debounced flame, NVS fire latch,
  teacher reset, alert indications, prioritized telemetry and server presence.
- Applied migrations `20261113084314` and `20261113085057` and deployed
  `classroom-ingest` version 2. The 56 existing public tables were preserved.
- SQL tests verified fire replay, latch history, alert resolution, and heartbeat-gap
  reconciliation. The operator reports a successful PlatformIO build.
- Follow the [Level 3 hardware and cloud checklist](LEVEL3_TEST_GUIDE.md).
  ESP32 safety simulations, alert timing, power interruption and latch recovery
  tests are pending. Physical panic and touch wiring remains unverified.
