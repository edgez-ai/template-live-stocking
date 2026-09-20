# Live Stocking

An Appwrite + Next.js + React Native + ESP32-S3 starter for authenticated device
onboarding, offline device mapping, Wi-Fi HaLow provisioning, and MQTT telemetry.
It mirrors the five-part structure of Hello Channels while giving each folder a
provisioning-specific responsibility.

[![Use this template](https://img.shields.io/badge/Use%20this-template-238636?style=for-the-badge&logo=github)](https://github.com/new?template_name=template-live-stocking&template_owner=edgez-ai)
[![Deploy on EdgeZ](https://img.shields.io/badge/Deploy%20on-EdgeZ-6c5ce7?style=for-the-badge)](https://appwrite.edgez.ai/console/deploy?repo=https%3A%2F%2Fgithub.com%2Fedgez-ai%2Ftemplate-live-stocking)

## Repository layout

| Folder | Purpose |
| --- | --- |
| `site/` | Read-only Next.js portal for sign-in, devices, and telemetry |
| `app/` | Expo app with an offline Organic Maps dashboard and BLE HaLow provisioning |
| `function/` | Trusted MQTT webhook that writes telemetry |
| `firmware/` | PlatformIO + ESP-IDF device client |
| `infra/` | Rerunnable Appwrite CLI installer |

The root `edgez.json` selects `appwrite.config.json` as the complete,
version-controlled solution plan. **Deploy on EdgeZ** applies the auth methods,
web and Android platforms, telemetry database and indexes, MQTT Function, Site,
variables, domains, and both source deployments after project selection.
Its `buildInstance` accepts `tiny`, `small`, `medium`, or `large`; this template
defaults to `tiny` for both Site and Function builds.

Open `live-stocking.code-workspace` in VS Code to work on all five folders.

## Provisioning flow

1. The ESP32 derives the serial as the 12 uppercase hexadecimal characters of
   its Wi-Fi MAC and advertises `PROV_<serial>` over BLE.
2. An operator creates an Appwrite account or signs in from web/mobile.
3. The mobile app strips the `PROV_` prefix, creates an Appwrite Device with that
   project-unique serial, then
   creates its one-time MQTT credential directly through the Devices API.
4. The app scans and selects a Wi-Fi HaLow network through the `halow-scan` BLE
   endpoint, then sends the MQTT credential and selected HaLow network to the
   firmware's `mqtt-config` and `halow-config` endpoints.
   Firmware connects to `mqtts://mqtt.edgez.ai:8883`, verifying the Let's Encrypt
   chain with ESP-IDF's trusted root bundle.
5. The device publishes JSON to
   `projects/<projectId>/devices/<serial>/telemetry/<channel>`. Appwrite's EMQX
   ACL allows that serial to publish only beneath its own telemetry/events
   topics and subscribe only beneath its own commands topic.
   Firmware publishes its internal chip temperature every 30 seconds on the
   `temp` channel as `{ "temperatureC": ..., "unit": "celsius", "sensor": "internal" }`.
6. Appwrite resolves the MQTT client and emits
   `devices.<deviceId>.mqtt.message.publish` to the Function.
7. The Function verifies the topic project and serial against the built-in
   device, then creates a telemetry row carrying its read permissions.
8. Web and mobile read permitted telemetry directly from TablesDB.

## Environment

The managed workspace injects:

```sh
APP_NAME=<workspace-name>
DOMAIN_SUFFIX=edgez.biz
APPWRITE_ENDPOINT=<appwrite-endpoint>
APPWRITE_PUBLIC_ENDPOINT=<optional-browser-safe-appwrite-endpoint>
APPWRITE_PROJECT_ID=<project-id>
APPWRITE_PROJECT_NAME=<dns-safe-project-name>
APPWRITE_API_KEY=<server-api-key>
```

Stable database and table IDs come directly from `appwrite.config.json`; the
template does not require a committed `.env.local` file.

`APPWRITE_ENDPOINT` configures the server-side CLI. Browser and mobile builds
use `APPWRITE_PUBLIC_ENDPOINT` when provided; otherwise a non-local HTTP
endpoint is automatically upgraded to HTTPS to prevent mixed-content errors.

All clients derive the Function URL as
`https://${APPWRITE_PROJECT_NAME}-${APP_NAME}.functions.${DOMAIN_SUFFIX}`.
The web/mobile clients also receive the public Appwrite endpoint, project ID,
and non-secret table IDs. The API key is used by `infra/` only. Appwrite's
server-level EMQX secret remains outside this application repository.

## Local validation

Install dependencies in `site/`, `app/`, `function/`, and `infra/`, then run:

```sh
(cd site && npm run typecheck && npm run build)
(cd app && npm run typecheck)
(cd function && npm run check)
(cd infra && npm run check)
(cd firmware && pio run)
```

Remote installation is intentionally separate: run `cd infra && npm run deploy`
only when you intend to provision or update Appwrite resources.
Run `cd infra && npm run clean` to remove this template's Site, Function,
database, proxy rule, and auth platforms. Cleanup preserves project users,
Devices, and project-wide authentication settings.

## Release builds

The separate `Build firmware` and `Build mobile app` GitHub Actions workflows
run when a GitHub Release is published and can each be started independently
with **Run workflow**. Firmware produces a PlatformIO archive; mobile produces
Android APK and AAB files. Workflow-run artifacts are always retained, while
release-triggered builds are also attached to the GitHub Release.
