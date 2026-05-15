# Firebase Realtime Migration Plan

Goal: move ELLA back to Firebase Realtime Database as the source of truth, while avoiding the old lag by keeping Firebase work out of AI/audio/checkup critical sections and by relying on valid NTP time before time-based features run.

## Current State

- `dashboard/index.html` is the recovered E.L.L.A Cloud Dashboard.
- The dashboard initializes Firebase RTDB at `https://ellacloudai-default-rtdb.firebaseio.com`.
- Migration progress in this work session:
    - Dashboard command writes now use Firebase `/commands/<key>`.
    - Dashboard reminders now persist to Firebase `/reminders`.
    - Dashboard live status/vitals now read Firebase `/status/snapshot` and `/status/vitals`.
    - Dashboard MQTT publish calls have been removed from active workflows.
    - Firmware now includes `Firebase_ESP_Client.h`, uses real Firebase objects, and has `FIREBASE_ENABLED = true`.
    - Firmware `setupFirebase()`, `checkRemoteCommands()`, `pushSensorDataToFirebase()`, `syncRemindersFromFirebase()`, `syncUserProfileFromFirebase()`, and `syncWithFirebase()` have been restored.
    - Firmware `MQTT_ENABLED = false`, so MQTT does not reconnect by default.
    - Telegram Alert relay migrated to Firebase:
        - Firmware `sendTelegramAlert()` now pushes to `/alerts`.
        - Server `server.js` monitors `/alerts` and forwards to Telegram.
        - Processed alerts are automatically removed by the server.
    - Webapp Hosting configured for `ellacloudai` project.
- `backend/ellabox.ino` contains older real Firebase logic that can be used as reference.

## Target Architecture

- Firebase RTDB is the persistent database and command source:
  - `/commands/*`
  - `/status/snapshot`
  - `/status/vitals`
  - `/status/imu`
  - `/reminders`
  - `/commands/userProfile`
  - `/settings/alertThresholds`
  - `/sleepLog`
  - `/conversations` or `/status/moodHistory`
- MQTT is optional fallback/diagnostic transport only.
- Firmware must not poll/push Firebase during:
  - `MODE_AI`
  - active TTS/audio playback
  - Node/Deepgram websocket startup
  - medical measurement/checkup
  - `networkIsQuiet()`
- NTP must be treated as a prerequisite for reminders, TLS-heavy services, and relative AI time reasoning.

## Migration Steps

1. **Preserve recovered web app**
   - Keep `dashboard/index.html` as the recovered app.
   - Optionally copy it to `dashboard/index.recovered.html`.

2. **Make dashboard Firebase-first**
   - Save reminders to `/reminders`.
   - Save commands to `/commands/<key>` through Firebase.
   - Keep MQTT publishes only as temporary fallback until firmware Firebase is confirmed.

3. **Re-enable Firebase library in firmware**
   - Restore `#include <Firebase_ESP_Client.h>`.
   - Remove dummy Firebase structs.
   - Set `FIREBASE_ENABLED = true`.
   - Use `FIREBASE_DATABASE_URL` and `FIREBASE_DB_SECRET` from `secrets.h`.

4. **Restore Firebase setup**
   - Implement `setupFirebase()` with auth token and RTDB URL.
   - Set reconnect Wi-Fi behavior.
   - Mark `firebaseReady` only after `Firebase.ready()`.

5. **Restore command polling safely**
   - Implement `checkRemoteCommands()` from the old Firebase code.
   - Poll only in Normal mode or safe guard/idle windows.
   - Clear command nodes after processing.
   - Commands to support: `emergency`, `wifiConfig`, `wifiReset`, `aiChat`, `aiInputMode`, `speak`, `checkup`, `meditate`, `breathe`, `systemMode`, `stopAudio`, `guard`, `autoAvoid`, `focusMode`, `imuReset`, `motor`, `speed`.

6. **Restore Firebase status/vitals**
   - `publishStatusSnapshot()` writes `/status/snapshot`.
   - `pushSensorDataToFirebase()` writes `/status/vitals` and optionally `/readings/<timestamp>`.
   - Keep pushes throttled and blocked during AI/audio/checkup.

7. **Restore reminders/profile/settings sync**
   - `syncRemindersFromFirebase()` reads `/reminders` into `cloudRemindersJson`.
   - `syncUserProfileFromFirebase()` reads `/commands/userProfile`.
   - `syncAlertThresholds()` reads `/settings/alertThresholds`.

8. **Reduce or remove MQTT**
   - After Firebase commands/status are working, remove dashboard MQTT command publishes.
   - Keep MQTT code in firmware disabled or only manually enabled for diagnostics.

9. **Verification**
   - Do not compile unless user asks.
   - Static check for duplicate Firebase function definitions.
   - Confirm dashboard writes `/commands/checkup`, `/commands/reminders`, etc.
   - Confirm firmware loops call Firebase only in safe sections.

## Resume Notes

If another assistant continues:

- Start by checking `git diff -- ellabox.ino dashboard/index.html FIREBASE_MIGRATION_PLAN.md`.
- Do not run compile unless the user explicitly allows it.
- The most important firmware area is around the disabled Firebase block near `// FIREBASE (DISABLED)`.
- The old reference implementation is in `backend/ellabox.ino` around:
  - `setupFirebase()`
  - `checkRemoteCommands()`
  - `pushSensorDataToFirebase()`
  - `syncUserProfileFromFirebase()`
  - `syncRemindersFromFirebase()`
- Be careful: active `ellabox.ino` has duplicate/old `#if 0` Firebase functions and many `firebaseReady` call sites. Avoid introducing duplicate function definitions.
