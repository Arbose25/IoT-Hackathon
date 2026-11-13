# Smart Classroom Level 1 test guide

## Before powering the board

Use the ESP32-S3-CAM N16R8 with the ESP32-S3 DevKitC-1 PlatformIO environment and its existing 16 MB flash and 8 MB PSRAM overrides. The camera is idle in Level 1. Keep the door disconnected.

| Signal | ESP32-S3 GPIO |
| --- | --- |
| DHT11 data | 1 |
| Flame sensor digital output, active low | 2 |
| RC522 SDA/SS, SCK, MOSI, MISO, RST | 41, 39, 40, 38, 42 |
| Onboard WS2812B data | 48 |
| Optional buzzer control | 47 |

Use a common ground and verify 3.3 V-compatible sensor signals. Power the RC522 from 3.3 V. If a flame module needs 5 V power, verify its digital output voltage and use level shifting if required. Refer to the [ESP32-S3 datasheet](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf) and your exact board revision's schematic or pinout. The firmware checks for duplicate assigned GPIOs at compile time. Verify GPIO 47 is available on your board before enabling it. The module name alone does not identify every carrier-board connection.

The buzzer defaults to disabled and leaves GPIO 47 untouched. Confirm whether it is an active module or a passive transducer and how much current it draws. Use GPIO 47 as a control signal to a suitable 3.3 V-compatible driver for loads that exceed the GPIO's capabilities. Use a shared ground, the specified supply for the buzzer, and flyback protection when the driver and inductive load require it. Leave it disconnected if these details are unknown.

## Configure, build, and upload

1. Open `D:\iot final\firmware` as a folder in VS Code. Open a PlatformIO terminal.
2. If `include/level1.local.h` is missing, copy `include/level1.example.h` to it. Put your two assigned UIDs inside the quotes. This file is Git-ignored. The public example's empty strings cause all cards to be reported as unknown. Keep the local header and the configured binary private because the binary contains the UIDs.
3. Leave `LEVEL1_BUZZER_MODE` at `0` until the board wiring and buzzer type are confirmed. Use `1` for an active buzzer or module and set `LEVEL1_BUZZER_ACTIVE_LOW` to `1` only for active-low hardware. Use `2` for passive 2.4 kHz PWM through a compatible active-high driver. Rebuild and upload after changing the header.
4. Connect the ESP32-S3 with a USB data cable. Run `pio device list`. Identify its COM port by checking which entry appears when you reconnect the cable.
5. Close any open Serial Monitor. Run the commands below from the firmware folder. Replace `COM13` with your actual port if it differs.

```powershell
pio run -e level1-stability
pio run -e level1-stability -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```

Wait for upload success and the Level 1 boot banner. If automatic entry into the bootloader fails, use the board's documented BOOT and RESET procedure. Send commands with Enter. LF and CRLF are supported, and command names are case-insensitive. Exit the monitor with Ctrl+C before uploading again.

## CPU 0 interrupt watchdog regression test

1. Flash the `level1-stability` environment. Confirm the boot banner includes
   `LEVEL 1 STABILITY BUILD - RMT DHT11`. This target starts only Level 1 tasks.
2. Keep Serial Monitor at 115200 open for at least five minutes after boot.
   Pass when `STATUS uptime=` reaches 300 and keeps increasing with no
   unexpected reboot, panic or watchdog timeout. Confirm fresh DHT11 readings
   continue about every five seconds.
3. During those five minutes, send `help`, `health`, and `tasks`, and tap each
   RFID card. Check `WATCHDOG: ACTIVE timeout_ms=15000 tasks=4/4`, both task
   cores, and the normal LED and configured buzzer responses.
4. With a prewired DHT11 data switch, interrupt the data signal for 15 seconds
   while leaving board power connected. Expect `SENSOR ERROR: DHT11` within
   12 seconds, steady magenta, continuing uptime and responsive Serial.
   Restore the signal. The first valid sample prints
   `DHT11 RECOVERY CHECK: 1/2 valid samples`. STATUS remains `dht=ERROR`
   with `temp=NA humidity=NA` and the LED stays magenta. The second consecutive
   valid sample, five seconds later, prints `DHT11 RECOVERED` and a valid
   `DHT11 READ`. The LED resumes the green heartbeat if no other faults remain.
   Wire the switch with power off.
5. Repeat a cold start with the data switch open, then reconnect the signal
   while the monitor is running. Check for continuous uptime and recovery.
   This covers the first-read window where the original log panicked.
6. Repeat the disconnect test. After the first recovery check, open the data
   switch again for at least one full five-second polling interval. Close it
   and expect another `1/2` check before recovery. No `DHT11 READ` should
   report humidity outside 20–90% or temperature outside 0–50 °C.

If a panic returns, save the full register dump and backtrace and retain the
matching `.pio/build/level1-stability/firmware.elf` before rebuilding. Compare
its SHA-256 prefix with the panic line before decoding with Xtensa `addr2line`.

## Hardware checklist

Record the observed results and timing. Each row passes when the expected behavior is visible on the board.

| Check | What to do | Expected result |
| --- | --- | --- |
| Startup and commands | Send `HeLp`, `health`, and `tasks`. | `SUCCESS: 4 FreeRTOS tasks created`. Help lists the original commands and the commands for the installed level. Health shows `CPU CORES: 2` and `WATCHDOG: ACTIVE timeout_ms=15000 tasks=4/4`. |
| Task timing | Run `tasks` twice, about ten seconds apart. | SensorTask, FlameTask and HealthTask report core 0. RFIDTask reports core 1. Counters grow by about 2, 100, 10 and 50, respectively. |
| DHT11 | Wait for the first samples, then watch for ten seconds. | `DHT11 READ temp=…C humidity=…%` about every five seconds. Status shows `dht=OK` with temperature and humidity. |
| Three RFID taps | Tap teacher, student, then an unmapped card individually. Present each for about half a second and move it away for at least half a second between taps. | `CARD role=TEACHER uid=<your teacher UID>`, `CARD role=STUDENT uid=<your student UID>`, then `CARD role=UNKNOWN`. The `cards` count rises by exactly three. |
| RFID hold and retap | Hold one card steadily near the reader for five seconds. Remove it for at least half a second and tap the same card again. | One event while held and one after the separate retap. One card at a time is supported for this check. |
| Buzzer command | Send `test buzzer` with mode `0`. Repeat after verifying the hardware, connecting the driver, enabling the correct mode, and uploading again. | Mode `0` prints `BUZZER TEST: DISABLED` and leaves GPIO 47 untouched. With the driver ready, `BUZZER TEST: ONE BEEP QUEUED` is followed by one short beep. If setup fails, the command prints `BUZZER TEST: INITIALIZATION FAILED`. |
| RFID buzzer patterns | With the correct mode enabled and the driver connected, repeat the three RFID taps. | One, two, and three short beeps. Holding a card creates one pattern. The status stream and commands continue during beeps. |
| Flame input | Use the module's safe test function or a verified, isolated 3.3 V test signal on GPIO 2 with the sensor output disconnected from the input. Toggle high, low, high. | `FLAME raw=1 state=CLEAR`, `FLAME raw=0 state=DETECTED`, then `FLAME raw=1 state=CLEAR`. Use no flame. The digital signal cannot diagnose a disconnected sensor. |
| Heartbeat and simulated faults | Watch the LED, send `test dht`, then `test rfid`, then `test clear`. | Green for 250 ms, off for 250 ms, repeating. On simulation, the LED turns steady magenta and Serial prints `SIMULATED SENSOR ERROR: DHT11` or `SIMULATED SENSOR ERROR: RC522`. `SIMULATED FAULTS CLEARED` restores the green heartbeat when both real sensors are healthy. |
| Real DHT11 loss and recovery | Use a prewired signal switch or isolated test fixture to interrupt DHT11 data while the board runs. Leave the signal open for 15 seconds, then restore it. | `SENSOR ERROR: DHT11` within 12 seconds and steady magenta. `test clear` leaves the real fault active. `DHT11 RECOVERED` follows two consecutive valid samples. |
| Real RC522 loss and recovery | Use a prewired signal switch or isolated test fixture to interrupt the RC522 MISO signal while the board runs. Leave it open for at least three seconds, then restore it. | `SENSOR ERROR: RC522` within three seconds and steady magenta. `RC522 RECOVERED` follows successful reader reinitialization and register checks. Verify it reads a card again. |
| Watchdog health | With the demonstration board isolated from actuators, run `health` again after at least 30 seconds of normal operation and after a sensor fault. | `WATCHDOG: ACTIVE timeout_ms=15000 tasks=4/4`, increasing uptime, and no subscription or feed errors. This checks configuration and normal operation. It does not induce a timeout. |

Power down before changing wiring. For live recovery checks, set up the switches while powered down and toggle only the prepared signals. Simulated faults alter status and the LED while hardware polling continues. Real faults take precedence, and the LED returns to green only after all real and simulated faults have cleared.

## Serial output format

This project defines the format below. The judges' Serial Commands Table was unavailable.

```text
STATUS uptime=28 dht=OK temp=25.0 humidity=52.0 rfid=OK flame=CLEAR cards=3 watchdog=ACTIVE door=UNLOCKED sim_dht=0 sim_rfid=0
```

Status prints once per second. Uptime is in seconds, temperature in degrees Celsius, and humidity in percent relative humidity. A sensor can report `STARTING`, `OK`, `ERROR`, or `SIM_ERROR`. Unavailable temperature and humidity are `NA`. Watchdog status is `PENDING`, `ACTIVE`, or `ERROR`. The configured watchdog reports timeouts without requesting a panic reset. Later firmware also prints `safety`. Level 3 changes the software door field when the fire latch changes and uses the teacher role for the simulated reset. No physical access control is connected.
