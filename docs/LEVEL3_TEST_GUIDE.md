# Level 3 — Safety prototype

First complete the [Level 2 guide](LEVEL2_TEST_GUIDE.md), including device registration
and a successful Level 2 build. For this stage, use the same PowerShell directory,
device and credentials. Close the Serial Monitor before upload.

```powershell
& $pio run -e esp32-s3-devkitc-1
& $pio run -e esp32-s3-devkitc-1 -t upload --upload-port COM13
& $pio device monitor --port COM13 --baud 115200
```

Replace `COM13` with your board port. Open the local IP shown on Serial. Verify the
dashboard shows firmware `level3`. Keep physical doors, relays and alarm systems
disconnected. Use synthetic inputs for heat, fire and panic tests.

## State and priorities

FIRE overrides PANIC, which overrides heat indicators. Temperature and humidity
are converted to heat index in Celsius using the [NOAA equation](https://www.wpc.ncep.noaa.gov/html/heatindex_equation.shtml).
Risk is the higher of heat index and ambient temperature. The prototype defaults
are 32 °C WARNING and 40 °C DANGER. Override with validated build flags
`CLASSROOM_HEAT_WARNING_C` and `CLASSROOM_HEAT_DANGER_C`. The 2 °C hysteresis
requires a reading below 38 °C to move from DANGER to WARNING and a later reading
below 30 °C to move to SAFE. An invalid reading retains WARNING/DANGER and displays
INVALID. With no prior heat hazard, an invalid reading displays FAULT.

The real DHT11 still polls every five seconds. Measure the three-second warning
target from delivery of a fresh reading. A physical threshold crossing between
polls can take longer. FIRE requires one second of continuous flame input.
A FIRE latch survives restart in NVS and sets the **software** door to UNLOCKED.
After one second of clear input, a new teacher card presentation may reset it.
The simulation follows the same reset path. PANIC uses a simulated three-second
hold because no panic/touch pin or hardware has been verified. Buzzer alerts
only run with the verified Level 1 buzzer configuration enabled.

## Evidence checklist

Build status in this workspace: **BLOCKED** by the Windows Xtensa compiler
launch error. The SQL tests passed for fire replay, fire and panic alert
creation, fire alert resolution, server presence, and outage reconciliation.
All ESP32 tests and latency targets below are **NOT TESTED**. The physical
panic input and touch calibration are **BLOCKED** pending verified hardware.

| Criterion | Test and expected evidence |
| --- | --- |
| L3-01/02 | Send `test heat warning` then `safety status`. Expect WARNING and valid heat index immediately after processing. Send `test heat danger` and check RISING and DANGER. |
| L3-03 | Verify the DANGER red blink and periodic buzzer if enabled. Send `test heat cool` twice, checking WARNING after the first and SAFE after the second. |
| L3-04 | Return to DANGER, send `test heat invalid`, and verify DANGER remains with INVALID quality. Use `test safety clear` to resume real samples. |
| L3-05 | Send `test panic`. After approximately three seconds expect `SAFETY: PANIC (SIMULATED)`, blue LED and a cloud panic event. Measure time from the local message to server receipt. Send `test safety clear` to clear simulated panic. |
| L3-06/07 | Send `test fire`. After about one second expect `SAFETY: FIRE, DOOR SOFTWARE UNLOCKED` and fast red LED. Tap teacher while active and expect `SAFETY: FIRE RESET DENIED`. Send `test fire clear`, wait one second, then tap student and unknown individually and check denial. Tap teacher and check `SAFETY: TEACHER RESET ACCEPTED`. |
| L3-08 | With regular cloud acknowledgments, power off the isolated prototype for 40 seconds. Query presence after 30 seconds from the last server heartbeat. Restore power and verify it rejoins Wi-Fi, records a new boot and reconciles an outage. |
| L3-09 | Send `test fire`, wait for the latch, then power-cycle. Expect `SAFETY: FIRE LATCH RESTORED, DOOR SOFTWARE UNLOCKED`. After clear input and a teacher tap, confirm the next restart clears the latch. |
| L3-10 | Send `test fire pulse` and `test panic pulse` separately. Each lasts 500 ms and should leave the corresponding latch clear. Repeat the heat trend sequence above. |
| L3-11 | Physical touch hardware and calibration are pending. No touch channel or panic GPIO is assigned. |
| L3-12 | Save sanitized Serial, dashboard and SQL evidence. After confirming mandatory tests, commit the verified revision, then use `git tag -a level-03 -m "Level 3 verified"` and `git push origin level-03`. |

Use `test fire clear` to end simulated flame input, then the teacher card after
the clear interval. `test safety clear` ends heat and panic simulation and
releases simulated flame input while respecting the FIRE latch.

## Cloud checks

Run in the Supabase SQL Editor and use your demo device UUID in place of the sample:

```sql
select * from public.classroom_device_presence
where device_id = '00000000-0000-0000-0000-000000000000';
```

Inspect `device_events` for fire, panic, reset and denial event IDs and separate
capture/receipt times. Inspect `safety_state_history` for the latch and door state,
`alerts` for a critical open alert and its resolution, and `outage_sessions`
for a server heartbeat gap and restoration. The inferred outage cause is approximate.
Presence is computed on query and does not require a scheduled job.

Repeat fire and panic with the hotspot disconnected for 20 seconds. Check the
local indicators and queue depth, then restore connectivity and inspect the server
event sequence and duplicate counts. Emergency records have a separate bounded
RAM queue and can preempt an ordinary retry between network requests. A request
already in progress can delay an emergency upload. Measure the three-second
online target on the board. A power loss clears RAM records, including unsent
events, while the stored FIRE latch remains.
