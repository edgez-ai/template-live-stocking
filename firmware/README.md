# Device firmware

The ESP32-S3 derives its Appwrite serial from the Wi-Fi station MAC as
`<12 uppercase hex digits>`, for example `AABBCCDDEEFF`. It advertises it as
`PROV_AABBCCDDEEFF` so the provisioning client can obtain the serial by stripping
the `PROV_` prefix before creating the Appwrite Device. It provisions Wi-Fi
through ESP-IDF BLE provisioning with the example proof of possession (PoP)
`abcd1234` and exposes a
custom `mqtt-config` endpoint. Send this JSON before applying Wi-Fi credentials:

```json
{
  "clientId": "device-uuid-from-appwrite",
  "username": "AABBCCDDEEFF",
  "password": "one-time-device-secret",
  "projectId": "049391cf-9119-4ff3-9b64-3d92b70bd612",
  "channel": "test"
}
```

The endpoint rejects credentials whose MQTT username does not exactly equal
the device-generated serial. The broker is pinned in firmware to
`mqtts://mqtt.edgez.ai:8883`. TLS validates
the broker's Let's Encrypt certificate chain through ESP-IDF's trusted root
certificate bundle; certificate verification is not disabled. The handler
validates the Appwrite serial syntax, stores the credential in NVS, and
publishes an online event to
`projects/<projectId>/devices/<serial>/telemetry/<channel>` after Wi-Fi and MQTT
connect. Every 30 seconds it also reads the ESP32-S3 internal chip temperature
and publishes a QoS 1 JSON message to
`projects/<projectId>/devices/<serial>/telemetry/temp`:

```json
{
  "temperatureC": 42.31,
  "unit": "celsius",
  "sensor": "internal"
}
```

The internal sensor measures chip temperature, not ambient room temperature.
The firmware also subscribes at QoS 1 to
`projects/<projectId>/devices/<serial>/commands/#`, matching Appwrite's EMQX
ACL. The Appwrite device must be created with `enabled: true`.

The onboard 128x64 SSD1306 OLED shows BLE provisioning, Wi-Fi, MQTT, and reset
status. Its Heltec WiFi LoRa 32 V3 connections are SDA 17, SCL 18, reset 21,
and active-low Vext power on GPIO 36.

To clear all saved provisioning data, press and hold the Heltec `USER/PRG`
button on GPIO 0 for five seconds after the firmware boots. The OLED shows a
countdown, the firmware erases the Wi-Fi and MQTT credentials from NVS, and
the board restarts advertising `PROV_<serial>` over BLE. Releasing the button
before five seconds cancels the reset and restores the current status display.

Production hardware should enable encrypted NVS/flash encryption. Credentials
must never be compiled into source or logged.
