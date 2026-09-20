# Mobile companion

The Expo app is the only device-onboarding client. It scans `PROV_` BLE
advertisements and derives the serial. After the operator selects a device,
the example proof of possession (PoP) `abcd1234` is prefilled to match the
firmware and the value shown on its OLED. The app establishes an ESP-IDF
Security 1 session, asks the ESP32 to scan nearby Wi-Fi networks, and lets the
operator select an SSID and enter its password. It then creates or
reuses the Appwrite Device, sends its one-time MQTT credential to `mqtt-config`,
and provisions Wi-Fi. It also uses Appwrite Auth and reads permitted telemetry
directly from TablesDB.

The `+ ADD` button in the dashboard header opens a full-screen, three-step flow
for selecting the BLE device, confirming its labeled name and PoP fields, and
selecting the scanned Wi-Fi network. The Wi-Fi password field is shown only for
secured networks.

The home screen presents each device as a card with its connectivity status and
latest internal chip temperature. Selecting a card opens the full-screen device
history view, where the temperature line chart can show the last 30 minutes,
1 hour, 6 hours, or 24 hours.
The detail view can also delete the Device after native destructive
confirmation. Appwrite removes its MQTT credential and route with the Device;
existing telemetry rows are retained.

New Devices are created with explicit read, update, and delete permissions for
the signed-in creator only. Telemetry inherits only that Device read permission.
Its Expo deep-link scheme is `edgez-devtools`, so application links begin with
`edgez-devtools://`.

BLE provisioning uses a native module and does not run in stock Expo Go.
`npm run android` starts Metro, forwards its port, and opens the project through
`edgez-devtools://` in EdgeZ Android DevTools on `127.0.0.1:5555`. It does not
run Gradle or build an APK. Override `ANDROID_SERIAL` or `EXPO_PORT` when needed.
