# Device firmware

The ESP32-S3 derives its Appwrite serial from the Wi-Fi station MAC as
`<12 uppercase hex digits>`, for example `AABBCCDDEEFF`. It advertises it as
`PROV_AABBCCDDEEFF` so the provisioning client can obtain the serial by stripping
the `PROV_` prefix before creating the Appwrite Device. It uses ESP-IDF BLE
provisioning as the secure transport, with the example proof of possession (PoP)
`abcd1234`. The app sends farm mesh settings, MQTT credentials, and optional
device coordinates together through `mqtt-config`. When the device has regular upstream
Wi-Fi, the app also uses ESP-IDF's standard Wi-Fi scan and provisioning calls.
Send this unified configuration JSON:

```json
{
  "clientId": "device-uuid-from-appwrite",
  "username": "AABBCCDDEEFF",
  "password": "one-time-device-secret",
  "projectId": "049391cf-9119-4ff3-9b64-3d92b70bd612",
  "channel": "status",
  "meshId": "edgez",
  "passphrase": "example-passphrase",
  "country": "US",
  "halowChannel": 27,
  "wifiUpstream": true,
  "latitude": 59.3293,
  "longitude": 18.0686
}
```

Latitude and longitude may both be `null` to clear the saved location. The
firmware stores coordinates in NVS and includes them in subsequent telemetry.
The country is a two-letter regulatory
code, and the channel is an S1G channel number supported in that country. The
mesh settings are stored in NVS. Without upstream Wi-Fi, firmware stops BLE
and starts the HaLow mesh directly. With upstream Wi-Fi, it waits for standard
ESP-IDF Wi-Fi provisioning to finish before starting the mesh. The Morse driver,
regulatory database, firmware, and board configuration
are packaged in `components/morse_halow`; public builds link its ESP32-S3
`libmorse.a` directly and do not require the internal MM-IoT source tree.

The MQTT endpoint rejects credentials whose username does not exactly equal
the device-generated serial. The broker is pinned in firmware to
`mqtts://mqtt.edgez.ai:8883`. TLS validates
the broker's Let's Encrypt certificate chain through ESP-IDF's trusted root
certificate bundle; certificate verification is not disabled. The handler
validates the Appwrite serial syntax, stores the credential in NVS, and
publishes an online event to
`projects/<projectId>/devices/<serial>/telemetry/<channel>` after Wi-Fi and MQTT
connect. Every 30 seconds it reads the HT-HC33 battery voltage using its
GPIO20 controlled divider and GPIO1 ADC input. It publishes a single QoS 1
JSON message to `projects/<projectId>/devices/<serial>/telemetry/status`:

```json
{
  "status": "online",
  "batteryVoltageMv": 3840,
  "unit": "millivolt",
  "latitude": 59.3293,
  "longitude": 18.0686
}
```

The ADC is calibrated and the measured divider voltage is doubled to recover
the battery voltage. A disconnected battery omits the voltage fields; status
and any configured location still publish.
The firmware also subscribes at QoS 1 to
`projects/<projectId>/devices/<serial>/commands/#`, matching Appwrite's EMQX
ACL. The Appwrite device must be created with `enabled: true`.

Release builds provide `live-stocking-flash.bin`, a merged bootloader, partition
table, and factory app image to flash at address `0x0`, and
`live-stocking-ota.bin`, the app-only image for an OTA updater. The partition
table has two OTA app slots and OTA data. Existing devices with the previous
factory-only partition table need the new partition table flashed before an OTA
image can be used. Flashing the merged image erases provisioning data in NVS;
provision the device again afterward. The firmware does not yet download or
apply OTA updates by itself.

This target is the Heltec HT-HC33. It has no dependency on the WiFi LoRa 32 V3
SSD1306 display or its I2C/Vext pins; provisioning, HaLow, MQTT, and reset status
are reported on the serial monitor at 115200 baud.

To clear all saved provisioning data, press and hold the Heltec `USER/PRG`
button on GPIO 0 for five seconds after the firmware boots. The serial log shows
a countdown, the firmware erases the HaLow and MQTT credentials from NVS, and
the board restarts advertising `PROV_<serial>` over BLE. Releasing the button
before five seconds cancels the reset and reports the current status over serial.

Production hardware should enable encrypted NVS/flash encryption. Credentials
must never be compiled into source or logged.
