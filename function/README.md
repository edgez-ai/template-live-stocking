# Provisioning Function

This Function subscribes to Appwrite's
`devices.*.mqtt.message.publish` event. Appwrite has already authenticated the
EMQX webhook and resolved its MQTT client to an internal device ID. The
Function validates `projects/<projectId>/devices/<serial>/telemetry/<channel>`
against the actual Appwrite Device and copies its read permissions to that
device's latest telemetry row. A JSON array is accepted for gateway batches. Each entry's
`clientId` selects the Appwrite device that owns that reading; remote devices
must belong to the gateway's farm. Every reading is written to measurement
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
2500 to 5000 with `unit: "millivolt"`, plus valid latitude and longitude.
Other telemetry channels keep their JSON payloads.

A publishing gateway may include `topology.links` in its own status entry. The
Function validates each peer against the gateway's farm and upserts one current
row per gateway-peer pair in `topology-links`. Peers omitted by the next report
are marked inactive. Each row keeps both the peer's `lastSeenAt` and the
gateway report's `reportedAt`; clients additionally reject reports older than
two minutes.
