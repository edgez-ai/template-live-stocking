# Mobile companion

The Expo app is the only device-onboarding client. It scans `PROV_` ESP32 and
nRF54 BLE advertisements and derives the serial. The nRF54 advertises service
`FFF0` so the app can route it to the direct `mqtt-config` endpoint. For ESP32, after the operator selects a device,
the example proof of possession (PoP) `abcd1234` is prefilled to match the
firmware and the value shown on its OLED. The app establishes an ESP-IDF
Security 1 session. The operator chooses whether the device has regular Wi-Fi
as its upstream connection. If so, the app uses the ESP32's built-in Wi-Fi scan
and provisioning flow. It creates or reuses the Appwrite Device, sends its
one-time MQTT credential, selected farm's country, HaLow channel, mesh ID,
passphrase, and optional device location together to `mqtt-config`. It also uses Appwrite Auth and
reads permitted telemetry directly from TablesDB.

The first signed-in session opens Settings to create a farm and its Appwrite
team. Each farm has a row in the `farms` table, and its creator owns the team.
Settings can create and edit farms through one form opened by the Create farm
or Edit farm button, switch the active farm, and manage its team. Farm owners
can invite members by email, optionally enter their names, and remove them.
The team list shows email and also shows a name when available. Invitees accept
through the web site's `/invite.html` page. New invitees set a password before
signing in to the mobile app. The invitation URL is calculated from `APP_NAME`,
the first eight characters of `APPWRITE_PROJECT_ID`, and `DOMAIN_SUFFIX`.
`APPWRITE_TEAM_INVITE_URL` can override the complete URL. Restart Metro after
changing these variables so its JavaScript bundle gets the new URL. Farm fields are
name, country, location, HaLow channel, mesh ID, and passphrase. The selected
farm ID is saved in Appwrite account preferences and restored on the next app
launch. The map and list show devices assigned to that farm; the list heading
uses its name. New devices receive the farm ID in their metadata and team-based
permissions. Existing devices without a farm ID remain unassigned.

The `+ ADD` button opens a four-step ESP32 flow: choose the BLE device, confirm its
name and PoP, choose whether to use upstream Wi-Fi, then confirm the farm mesh
settings. The Wi-Fi branch scans nearby networks and accepts SSID/password.
Provisioning sends optional device coordinates through the existing BLE
`mqtt-config` endpoint. The device stores them and reports them in unified status telemetry;
the app uses the latest telemetry coordinates for map markers.
For nRF54 the flow has three steps. The app connects over BLE without a PoP or
pairing code, skips upstream Wi-Fi selection, and writes the same configuration
to its encrypted `mqtt-config` GATT characteristic. Device GPS is an additional
location choice for nRF54; it uses fixes from the GPS attached to the device.
The nRF54 stores the MQTT credential and HaLow profile. Its HaLow beacon uses
the MQTT client ID (the Appwrite Device ID) as its user ID and includes GPS and
IMU sensor data when available. MQTT telemetry publishing is not yet implemented
on that target.

The signed-in home screen opens with a full-screen Organic Maps view from
`@edgez/react-native-sdk`. The top-right menu switches between map and list
views or signs out; `+ ADD` remains beside it. Operators can download the
current map region once and continue viewing it offline; device coordinates
are displayed as map nodes when latitude and longitude are present. List view
shows the selected farm name and each device as a card with its connectivity
status and latest battery voltage. Selecting a card opens the full-screen device
history view, where the battery voltage line chart can show the last 30 minutes,
1 hour, 6 hours, or 24 hours. The detail view also lists direct HaLow peers from
the current-state `topology-links` table, including radio MAC, RSSI when
available, and last-seen time. It hides links whose gateway report is more than
two minutes old.
The detail view can also delete the Device after native destructive
confirmation. Appwrite removes its MQTT credential and route with the Device;
existing telemetry rows are retained.

New Devices grant read access to farm team members and update/delete access to
farm team owners. Telemetry inherits the Device read permission.
Its Expo deep-link scheme is `edgez-devtools`, so application links begin with
`edgez-devtools://`.

BLE provisioning and Organic Maps use native modules and do not run in stock
Expo Go. A development client must be rebuilt after adding these modules.
`npm run android` starts Metro, forwards its port, and opens the project through
`edgez-devtools://` in EdgeZ Android DevTools on `127.0.0.1:5555`. It does not
run Gradle or build an APK. Override `ANDROID_SERIAL` or `EXPO_PORT` when needed.
