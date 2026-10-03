# Provisioning Function

This Function subscribes to Appwrite's
`devices.*.mqtt.message.publish` and `devices.*.delete` events. Appwrite has already authenticated the
EMQX webhook and resolved its MQTT client to an internal device ID. The
Function validates `projects/<projectId>/devices/<serial>/telemetry/<channel>`
against the actual Appwrite Device and copies its read permissions to that
device's latest telemetry row. A JSON array is accepted for gateway batches. Each entry's
`clientId` selects the Appwrite device that owns that reading; remote devices
must report that MQTT publisher as their `gatewayDeviceId`. Relayed rows reference the MQTT topic device
through `gatewayDeviceId`; direct telemetry clears that optional field. Every reading is written to measurement
`device_<deviceId>` in the project's Time Series Store, with numeric sensors as
named fields such as `sensor_temperature` and `sensor_battery_voltage`.
Latitude and longitude are stored as numeric `lat` and `lon` fields compatible
with Flux's built-in `experimental/geo` package. The latest TablesDB row stores
the same fix as an indexed `location` point in `[longitude, latitude]` order for
native spatial distance queries. The Function also patches that point onto the
Device's native `location` field and projects native `markerType` and
`markerColor` into the row for direct map rendering. Repeated `clientId` entries remain separate time-series
points, while TablesDB keeps only the last reading and status for each device.
A legacy single JSON object follows the same path for the publishing device.
Sensor type 12 carries battery voltage in volts; types 3 and 4 carry latitude
and longitude. Other application telemetry channels keep their JSON payloads.

When an Appwrite Device is deleted, the same Function removes its deterministic
latest-telemetry row. This releases the unique HaLow MAC mapping before a device
with the same serial and radio is recreated; deleting an already-absent row is
treated as success.

System status, topology, and HaLow RF metrics are handled by Appwrite's Devices
service and never enter this Function. Clients request topology explicitly with
`includeTopology=true` and fetch RF history separately from
`/devices/{deviceId}/halow-metrics`.
