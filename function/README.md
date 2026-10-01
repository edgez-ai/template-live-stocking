# Provisioning Function

This Function subscribes to Appwrite's
`devices.*.mqtt.message.publish` and `devices.*.delete` events. Appwrite has already authenticated the
EMQX webhook and resolved its MQTT client to an internal device ID. The
Function validates `projects/<projectId>/devices/<serial>/telemetry/<channel>`
against the actual Appwrite Device and copies its read permissions to that
device's latest telemetry row. A JSON array is accepted for gateway batches. Each entry's
`clientId` selects the Appwrite device that owns that reading; remote devices
must belong to the gateway's farm. Relayed rows reference the MQTT topic device
through `gatewayDeviceId`; direct telemetry clears that optional field. Every reading is written to measurement
`device_<deviceId>` in the project's Time Series Store, with numeric sensors as
named fields such as `sensor_temperature` and `sensor_battery_voltage`.
Latitude and longitude are stored as numeric `lat` and `lon` fields compatible
with Flux's built-in `experimental/geo` package. The latest TablesDB row stores
the same fix as an indexed `location` point in `[longitude, latitude]` order for
native spatial distance queries. It also projects Device metadata's `icon` and
`markerColor` into the row for direct map rendering. Repeated `clientId` entries remain separate time-series
points, while TablesDB keeps only the last reading and status for each device.
A legacy single JSON object follows the same path for the publishing device.
The unified `status` payload may include an integer `batteryVoltageMv` from
0 to 10000 with `unit: "millivolt"`, plus valid latitude and longitude.
Other telemetry channels keep their JSON payloads.

When an Appwrite Device is deleted, the same Function removes its deterministic
latest-telemetry row. This releases the unique HaLow MAC mapping before a device
with the same serial and radio is recreated; deleting an already-absent row is
treated as success.

Any entry may include `topology.links`, including a relayed beacon entry whose
peer is the observing relay. The Function validates each peer against the
reporting device's farm and upserts one current row per reporting-device/peer
pair in `topology-links`. Peers omitted by the next report are marked inactive.
Each row keeps both the peer's `lastSeenAt` and the report's `reportedAt`;
clients additionally reject reports older than two minutes.
