# Device firmware

## KeepTeen nRF54L15

Build the nRF54L15/MM6108 target with:

```sh
pio run -e keepteen-nrf54l15
```

This target uses the MM6108 archive in the shared prebuilt Morse Micro Zephyr
package at `modules/mm-iot-zephyr-prebuilt` and the nRF54 application port in
`nrf54/`.
The package contains `libmorse.a`, public headers, radio firmware, and the
KeepTeen board calibration file; it contains no Morse implementation source.
The nRF54 uses the EdgeZ provisioning BLE service and advertises a `PROV_` name
with its 12-character serial. Its separate GATT service accepts
`mqtt-config` directly without BLE pairing, ESP-IDF provisioning, Wi-Fi setup,
or PoP. The app sends the farm country, 1 MHz channel frequency, mesh ID,
passphrase, optional device location or device GPS selection, and MQTT credential.
The firmware saves these settings and reboots into the selected HaLow profile.
Before acknowledging provisioning, it reads both saved MQTT and HaLow records
back from NVS and returns the request's confirmation ID over the BLE status
characteristic. It waits for the mobile app to disconnect before rebooting.
Its sensor beacon uses the MQTT `clientId`/Appwrite Device UUID as its user ID,
with IMU, GPS, and battery voltage sensor values. The KeepTeen board's
`ADC_VBAT` on P1.13 uses SAADC channel 6 and a 100 kΩ + 100 kΩ divider;
P1.14 (`ADC_VBAT_EN`) powers the divider only while sampling. Battery voltage
is resampled every 15 seconds and the cached HaLow vendor IE is refreshed while
the interface is active, so updates do not require a reboot. It is reported in
volts in the HaLow beacon. MQTT telemetry
publishing has not yet been ported to the nRF54 target. BLE provisioning stays
off on subsequent boots. Short press the USER button to enable BLE provisioning
again; after the next successful configuration it turns off again. Hold USER
for five seconds, then release it to erase the NVS storage partition and reboot
unprovisioned. This clears the saved HaLow and MQTT configuration and any old BLE bonds.

## Heltec HT-HC01 nRF54L15

Build the nRF54L15/MM6108 HT-HC01 target with:

```sh
pio run -e seeed-xiao-nrf54l15-hc01
```

This target uses the same application, MM6108 radio firmware, and prebuilt
Morse stack as FGH100M. Its only carrier-specific inputs are
`zephyr/boards/xiao_nrf54l15_hc01.overlay` and the
`bcf_HC01_V2_3V3.mbin` calibration selected by `zephyr/hc01.conf`.

## FGH200M nRF54L15

Build the nRF54L15/MM8108 target with:

```sh
pio run -e seeed-xiao-nrf54l15-fgh200m
```

This environment mirrors the FGH200M target in `edge-device-nrf54`. It uses the
FGH200M carrier overlay in `zephyr/boards/xiao_nrf54l15_fgh200m.overlay`, the
MM8108 radio firmware, and the temporary MF15457 calibration fallback selected
by `zephyr/fgh200m.conf`. Its MM8108 source-free library, radio firmware, and
BCF share `modules/mm-iot-zephyr-prebuilt` with the FGH100M package.

## Heltec HT-HC33

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
connect. The HT-HC33 queues decoded remote HaLow beacon readings in arrival
order, including repeated readings from the same `clientId`. It publishes a
batch when five remote readings are queued or 30 seconds have passed, whichever
comes first. Each batch also includes a fresh HT-HC33 battery reading from its
GPIO20 controlled divider and GPIO1 ADC input. Its status entry also includes
the direct HaLow peers observed in the last two minutes, keyed by the peer's
Appwrite device ID and radio MAC, with RSSI when the Morse driver provides it.
The QoS 0 JSON payload sent to
`projects/<projectId>/devices/<serial>/telemetry/status` is an array:

```json
[
  {
    "clientId": "11111111-1111-4111-8111-111111111111",
    "status": "online",
    "batteryVoltageMv": 3840,
    "unit": "millivolt",
    "latitude": 59.3293,
    "longitude": 18.0686,
    "topology": {
      "links": [{
        "peerId": "22222222-2222-4222-8222-222222222222",
        "peerRadioMac": "02:00:00:00:00:02",
        "ageMs": 1250,
        "rssi": -61
      }]
    }
  },
  {
    "clientId": "22222222-2222-4222-8222-222222222222",
    "status": "online",
    "batteryVoltageMv": 3700,
    "unit": "millivolt",
    "sensors": [{ "type": 12, "value": 3.7 }]
  }
]
```

The ADC is calibrated and the measured divider voltage is doubled to recover
the battery voltage. A disconnected battery omits the voltage fields; status
and any configured location still publish. The in-memory queue holds up to 32
remote readings while waiting to publish; overflow is logged and drops the new
reading.
The firmware also subscribes at QoS 1 to
`projects/<projectId>/devices/<serial>/commands/#`, matching Appwrite's EMQX
ACL. The Appwrite device must be created with `enabled: true`.

Release builds provide `live-stocking-flash.bin`, a merged bootloader, partition
table, and factory app image to flash at address `0x0`, and
`live-stocking-ota.bin`, the app-only image for an OTA updater. The partition
table has two OTA app slots and OTA data. Existing devices with the previous
factory-only partition table need the new partition table flashed before an OTA
image can be used. Flashing the merged image erases provisioning data in NVS;
provision the device again afterward.

The HT-HC33 subscribes to
`projects/<projectId>/devices/<serial>/commands/ota`. A command contains an
HTTPS `url` and unique `requestId`. Firmware queues the request outside the MQTT
callback, downloads it with the ESP-IDF certificate bundle, installs it in the
inactive OTA slot, and restarts only after ESP-IDF validates the image. OTA
progress is published on `telemetry/ota`. Normal gateway status telemetry and
OTA progress both include the running ESP-IDF application `firmwareVersion`.

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
