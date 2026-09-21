# Live Stocking project guide

Read this file before changing this repository.

## Product

Live Stocking is an authenticated Appwrite example for onboarding ESP32-S3
devices and viewing MQTT telemetry. Web and mobile use Appwrite Auth and access
TablesDB directly under row-level permissions. The Function is reserved for
trusted MQTT ingestion and is the only MQTT-to-TablesDB writer.

## Repository layout

| Path | Responsibility |
| --- | --- |
| `site/` | Read-only Next.js telemetry portal. Never create Devices here. |
| `app/` | Expo + React Native BLE provisioning client; the only Device creator. |
| `function/` | Appwrite Function; the only MQTT-to-TablesDB writer. |
| `firmware/` | PlatformIO + ESP-IDF device client and BLE provisioning. |
| `edgez.json` | EdgeZ solution and workspace manifest. |
| `appwrite.config.json` | Declarative auth, platform, database, Function, and Site plan. |
| `infra/` | Reproducible Appwrite CLI installer. |
| `.github/workflows/` | Independent firmware and Android release/manual builds. |
| `README.md` | Setup guide and environment contract. |

Keep these boundaries. Never expose `APPWRITE_API_KEY` to clients. Authenticated
web and mobile clients read TablesDB directly; only the Function writes MQTT
telemetry.

Only mobile calls the Device API's create and credential endpoints. Web and
mobile may list Devices and query `telemetry` directly with the active Appwrite
session. Serial is the MQTT username and is unique within the Appwrite project.
MQTT telemetry inherits only the built-in device's read permissions.

Firmware derives serial as 12 uppercase Wi-Fi MAC hex characters, advertises
`PROV_<serial>`, and accepts its Appwrite MQTT credential through the custom
BLE `mqtt-config` endpoint. It connects only to `mqtts://mqtt.edgez.ai:8883`,
publishes under `projects/<projectId>/devices/<serial>/telemetry/#`, and
subscribes under `projects/<projectId>/devices/<serial>/commands/#`.
It reads the HT-HC33 battery ADC every 30 seconds and publishes one `status`
telemetry message with online state, optional battery voltage in millivolts,
and optional coordinates stored on the device during BLE provisioning.

## Environment

The workspace supplies `APP_NAME`, `DOMAIN_SUFFIX`, `APPWRITE_ENDPOINT`,
`APPWRITE_PROJECT_ID`, `APPWRITE_PROJECT_NAME`, and `APPWRITE_API_KEY`.
Stable resource IDs come from the committed `appwrite.config.json`.
`APPWRITE_PUBLIC_ENDPOINT` may override the client endpoint. Non-local HTTP
client endpoints must be normalized to HTTPS; the CLI may retain its injected
server-side endpoint.

## Validation

- Web: `cd site && npm run typecheck && npm run build`
- Mobile: `cd app && npm run typecheck`
- Function: `cd function && npm run check`
- Infrastructure: `cd infra && npm run check`
- Firmware: `cd firmware && pio run`

`cd app && npm run android` must start Metro and open the project in the remote
EdgeZ Android DevTools client at `127.0.0.1:5555`. Do not run a local Gradle or
Expo native build unless the user explicitly requests one.

Only run `infra/npm run deploy` when the user explicitly asks to change remote
Appwrite state.
`cd infra && npm run clean` deletes the template's deterministic remote
resources but deliberately preserves project users, Devices, and global auth
method settings. Do not execute it unless the user explicitly requests cleanup.
