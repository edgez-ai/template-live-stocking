# Next.js operator portal

The Next.js client uses Appwrite email/password sessions. It reads only the
latest device state and topology from TablesDB, then queries historical sensor
fields from the project Time Series Store through the Appwrite API. Appwrite
permissions restrict both data paths to the signed-in user. The Function is not
used for dashboard reads.
The Site lists Devices but never creates them or MQTT credentials. Onboarding
is exclusively available from the React Native app over BLE.

On desktop, the authenticated dashboard uses a master-detail layout with the
device list on the left and the selected device's current reading, multi-sensor
history chart, statistics, and recent InfluxDB history on the right. Sensor
fields are discovered dynamically and operators can overlay any combination in
one chart. On mobile, devices are
shown as app-style cards and selecting one opens a full-screen detail view.
Both layouts require an explicit confirmation before deleting the selected
device through the authenticated Appwrite Devices API. The detail view also
draws the selected device's direct HaLow links from the current-state
`topology-links` table. Only active rows whose gateway report arrived within
the last two minutes are displayed.
The authenticated `/topology` page renders those same recent links as an
interactive force-directed network graph. Operators can pan, zoom, drag and
inspect devices, filter the graph by farm, compare RF RSSI, and see unresolved
HaLow peers alongside provisioned devices without requiring a Neo4j database.
HT-HC33 details also show the running firmware version and offer MQTT OTA from
the latest release of `APPWRITE_VCS_REPOSITORY_URL`, the repository used for
the current Appwrite Site deployment.
Appwrite filters device reads by the permissions assigned at creation, so the
portal only lists Devices readable by the signed-in creator.
