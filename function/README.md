# Provisioning Function

This Function subscribes to Appwrite's
`devices.*.mqtt.message.publish` event. Appwrite has already authenticated the
EMQX webhook and resolved its MQTT client to an internal device ID. The
Function validates `projects/<projectId>/devices/<serial>/telemetry/<channel>`
against the actual Appwrite Device and copies its read permissions to the new
telemetry row. A JSON array is accepted for gateway batches. Each entry's
`clientId` selects the Appwrite device that owns that reading; remote devices
must belong to the gateway's farm. Repeated `clientId` entries create separate
rows in arrival order. A legacy single JSON object still creates one row for
the publishing device.
The unified `status` payload may include an integer `batteryVoltageMv` from
2500 to 5000 with `unit: "millivolt"`, plus valid latitude and longitude.
Other telemetry channels keep their JSON payloads.

A publishing gateway may include `topology.links` in its own status entry. The
Function validates each peer against the gateway's farm and upserts one current
row per gateway-peer pair in `topology-links`. Peers omitted by the next report
are marked inactive. Each row keeps both the peer's `lastSeenAt` and the
gateway report's `reportedAt`; clients additionally reject reports older than
two minutes.
