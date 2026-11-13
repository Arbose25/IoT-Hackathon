# Level 2 — Wi-Fi and telemetry

Level 1 is reported complete by the operator. Use the same ESP32-S3 N16R8 environment
and private Level 1 configuration. No new pins are used. The official judging PDF
was unavailable locally. This guide follows the supplied implementation specification.

## Register the demo device

1. Open Windows PowerShell. Generate a unique 256-bit device token and its SHA-256 digest.
   Keep the token private. Copy only the digest to the SQL Editor.

   ```powershell
   $bytes = New-Object byte[] 32
   $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
   $rng.GetBytes($bytes)
   $rng.Dispose()
   $token = ([BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
   $sha = [Security.Cryptography.SHA256]::Create()
   $digest = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($token)))).Replace('-', '').ToLowerInvariant()
   $sha.Dispose()
   $digest
   ```

2. In the Supabase project SQL Editor, paste the contents of
   `supabase/demo/register_level2_device.sql`. Replace the digest placeholder with
   the output above. Run it once. Copy the returned device UUID. The room code is
   `A-101`. The demo uses fictional school and classroom records.
3. In the private PowerShell window, enter `$token` when you are ready to fill
   the setup form. Store the token securely for re-provisioning. If it is exposed,
   revoke the affected credential in `device_credentials` and register a new token.

## Build, upload and connect

From the normal PowerShell terminal where your Level 1 build worked:

```powershell
cd 'D:\iot final\firmware'
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e level2
```

Proceed only after PlatformIO reports success. Find the board with
`& $pio device list`. Replace `COM13` with the actual port:

```powershell
& $pio run -e level2 -t upload --upload-port COM13
& $pio device monitor --port COM13 --baud 115200
```

1. On first boot, look for `WIFI SETUP: SmartClass-DEMO password=… http://192.168.4.1`.
   Connect your phone or computer to that Wi-Fi network using the displayed password.
   Keep the password out of evidence screenshots. Set `CLASSROOM_TEAM` as a quoted
   build definition if your team uses a different short identifier.
2. Open `http://192.168.4.1`. Enter your 2.4 GHz hotspot SSID and password,
   `A-101`, the registered device UUID and its raw token. Use a trusted lab network.
   The form is local HTTP over the password-protected setup AP. The device stores
   provisioning data in NVS. Physical flash protection depends on board provisioning.
3. Rejoin the hotspot. Look for `WIFI CONNECTED: <IP> http://<name>.local cloud=waiting`.
   Open the printed IP if the hostname cannot be resolved. Allow time for NTP before
   expecting HTTPS and `CLOUD ACK: ACCEPTED`. The status page is intended for a
   private lab LAN and shows role, uptime and telemetry without card UIDs or tokens.
4. Send `wifi setup` to re-enter setup at runtime. GPIO0 was not verified, so
   BOOT-button provisioning is pending. Use the private AP link **Private card demo**
   to inspect the latest UID. The same route should return 404 over the LAN.
   Resubmit valid settings to close the setup AP.

Build status in this workspace: **BLOCKED** by the Windows Xtensa compiler
launch error. The automated server checks passed for accepted telemetry,
duplicate events, sequence conflicts, wrong credentials, device and room
isolation, and revocation. The Edge handler tests mock the database request.
All physical and end-to-end checks in the table are **NOT TESTED**. The
physical BOOT hold is **BLOCKED** pending board verification.

## Manual evidence checklist

| Criterion | Action and expected result |
| --- | --- |
| L2-01/02 | Connect to the password-protected AP. Confirm the form accepts valid settings, then returns the printed IP. Restart and verify settings persist. |
| L2-03/04 | Open the dashboard and tap each test card. Role and scan count update within two seconds. Hold one card five seconds and confirm only one increment. Verify the private demo UID view on the AP only. |
| L2-05 | With Wi-Fi and cloud healthy, look for an acknowledgment about every five seconds. Check rows as described below. |
| L2-06 | Send `test resend`. Expect `CLOUD ACK: DUPLICATE`. Confirm that the resent event ID produced no extra sensor or health row. New periodic telemetry may still add rows. |
| L2-07 | Turn the hotspot off for 20 seconds. Check that Level 1 sensors and uptime continue and `queue status` rises. Restore the hotspot and measure time to reconnect and drain. Target reconnect within 30 seconds. |
| L2-08 | Send `wifi setup`, submit an incorrect hotspot password, then reconnect to the AP and correct it. Confirm recovery with continuous uptime. Physical BOOT hold remains unverified. |
| L2-09 | Block only the Supabase endpoint for 20 seconds on a controlled test network while keeping Wi-Fi connected. Look for a growing queue and retry codes in `cloud status`. Restore access and check that backlog drains without duplicate IDs. |
| L2-10 | Send an HTTPS POST to the endpoint without a device token. Expect HTTP 401. For TLS rejection, use a controlled invalid certificate or DNS test and confirm no upload is acknowledged. |
| L2-11 | Save sanitized Serial output and table evidence for the rows above. After all mandatory checks pass, commit the verified revision and run `git tag -a level-02 -m "Level 2 verified"` and `git push origin level-02`. |

The bounded queue holds 96 pending records plus one in flight. It is RAM-only
and discards the oldest pending sample when full. Capture time stays null with
`clock_quality=unknown` until the clock is synchronized. Supabase stores a separate
receipt time. Power interruption loses the Level 2 RAM queue.

## Supabase rows

- `device_boots`: device ID, boot ID, reset reason, firmware version and receipt time.
- `device_events`: one event per accepted event ID and per device/boot/sequence.
- `sensor_readings`: event ID, boot ID, sequence, capture time, clock quality,
  receipt time, temperature, humidity, flame, DHT/RFID health and RSSI.
- `device_health`: boot ID, uptime, free heap, watchdog, DHT/RFID health and receipt time.
- `devices`: registered room association, firmware version and last validated contact.

Run `node supabase/tests/classroom_ingest_edge.cjs` at the repository root for the
Edge handler tests. Run `supabase/tests/classroom_ingest.sql` in the SQL Editor for
the transactional backend checks. The test transaction rolls back its fixtures.
