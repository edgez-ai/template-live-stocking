# Provisioning Function

This Function subscribes to Appwrite's
`devices.*.mqtt.message.publish` event. Appwrite has already authenticated the
EMQX webhook and resolved its MQTT client to an internal device ID. The
Function validates `projects/<projectId>/devices/<serial>/telemetry/<channel>`
against the actual Appwrite Device and copies its read permissions to the new
telemetry row.
