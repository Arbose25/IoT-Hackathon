# Smart Classroom IoT Hackathon Progress

## Level 1 - In progress

Verified on the ESP32-S3:

- Compiled the firmware with PlatformIO and uploaded it to the board.
- Confirmed that the FreeRTOS sensor and health tasks run on core 0 and the RFID task runs on core 1.
- Verified the Serial commands `help`, `health`, and `tasks`.

Remaining:

- Integrate and test the DHT11 temperature and humidity sensor.
- Integrate and test the RC522 RFID reader.
- Integrate and test the flame sensor.
- Implement the LED heartbeat and watchdog.
- Add sensor fault recovery and verify that readings resume after a fault.
- Run final Level 1 integration tests on the ESP32-S3.

## Level 2 - Not started

## Level 3 - Not started
