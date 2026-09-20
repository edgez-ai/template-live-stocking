# Next.js operator portal

The Next.js client uses Appwrite email/password sessions and directly accesses
TablesDB. Appwrite row permissions restrict device and telemetry visibility to
the signed-in owner. The Function is not used for dashboard reads.
The Site lists Devices but never creates them or MQTT credentials. Onboarding
is exclusively available from the React Native app over BLE.

On desktop, the authenticated dashboard uses a master-detail layout with the
device list on the left and the selected device's current temperature, history
chart, statistics, and recent telemetry on the right. On mobile, devices are
shown as app-style cards and selecting one opens a full-screen detail view.
Both layouts require an explicit confirmation before deleting the selected
device through the authenticated Appwrite Devices API.
Appwrite filters device reads by the permissions assigned at creation, so the
portal only lists Devices readable by the signed-in creator.
