import { config, ensure, run } from "./appwrite.mjs";

function stringColumn(key, size) {
  const common = ["--database-id", config.databaseId, "--table-id", config.telemetryTableId];
  ensure(`telemetry.${key} column`,
    ["tables-db", "get-column", ...common, "--key", key],
    ["tables-db", "create-string-column", ...common, "--key", key, "--size", size, "--required", "true"]);
}

export function installDatabase() {
  ensure("database",
    ["tables-db", "get", "--database-id", config.databaseId],
    ["tables-db", "create", "--database-id", config.databaseId, "--name", `${config.name} database`]);

  const farms = ["--database-id", config.databaseId, "--table-id", config.farmTableId];
  ensure("farms table",
    ["tables-db", "get-table", ...farms],
    ["tables-db", "create-table", ...farms, "--name", "Farms", "--permissions", 'create("users")', "--row-security", "true"]);
  run(["tables-db", "update-table", ...farms, "--permissions", 'create("users")', "--row-security", "true"]);
  for (const [key, size] of [["name", "128"], ["country", "2"], ["location", "128"], ["meshId", "32"], ["teamId", "36"], ["ownerId", "36"]]) {
    ensure(`farms.${key} column`,
      ["tables-db", "get-column", ...farms, "--key", key],
      ["tables-db", "create-string-column", ...farms, "--key", key, "--size", size, "--required", "true"]);
  }
  ensure("farms.halowChannel column",
    ["tables-db", "get-column", ...farms, "--key", "halowChannel"],
    ["tables-db", "create-integer-column", ...farms, "--key", "halowChannel", "--min", "1", "--max", "255", "--required", "true"]);
  ensure("farms.meshPassphrase column",
    ["tables-db", "get-column", ...farms, "--key", "meshPassphrase"],
    ["tables-db", "create-string-column", ...farms, "--key", "meshPassphrase", "--size", "63", "--required", "true", "--encrypt", "true"]);
  ensure("farms team index",
    ["tables-db", "get-index", ...farms, "--key", "team-id"],
    ["tables-db", "create-index", ...farms, "--key", "team-id", "--type", "key", "--columns", "teamId"]);

  const telemetry = ["--database-id", config.databaseId, "--table-id", config.telemetryTableId];
  ensure("telemetry table",
    ["tables-db", "get-table", ...telemetry],
    ["tables-db", "create-table", ...telemetry, "--name", "Telemetry", "--row-security", "true"]);
  run(["tables-db", "update-table", ...telemetry, "--row-security", "true"]);
  stringColumn("deviceId", "36");
  stringColumn("serial", "36");
  stringColumn("channel", "160");
  stringColumn("topic", "320");
  stringColumn("payload", "10000");
  stringColumn("receivedAt", "40");
  ensure("telemetry received-at index",
    ["tables-db", "get-index", ...telemetry, "--key", "received-at"],
    ["tables-db", "create-index", ...telemetry, "--key", "received-at", "--type", "key", "--columns", "receivedAt", "--orders", "DESC"]);
  ensure("telemetry device index",
    ["tables-db", "get-index", ...telemetry, "--key", "device-id"],
    ["tables-db", "create-index", ...telemetry, "--key", "device-id", "--type", "key", "--columns", "deviceId"]);
  ensure("telemetry device history index",
    ["tables-db", "get-index", ...telemetry, "--key", "device-history"],
    ["tables-db", "create-index", ...telemetry, "--key", "device-history", "--type", "key", "--columns", "deviceId", "receivedAt", "--orders", "ASC", "ASC"]);

  const topology = ["--database-id", config.databaseId, "--table-id", config.topologyTableId];
  ensure("topology links table",
    ["tables-db", "get-table", ...topology],
    ["tables-db", "create-table", ...topology, "--name", "Topology links", "--row-security", "true"]);
  run(["tables-db", "update-table", ...topology, "--row-security", "true"]);
  for (const [key, size] of [["farmId", "36"], ["gatewayDeviceId", "36"], ["gatewaySerial", "36"], ["peerDeviceId", "36"], ["peerSerial", "36"], ["peerRadioMac", "17"], ["lastSeenAt", "40"], ["reportedAt", "40"]]) {
    ensure(`topology.${key} column`,
      ["tables-db", "get-column", ...topology, "--key", key],
      ["tables-db", "create-string-column", ...topology, "--key", key, "--size", size, "--required", "true"]);
  }
  ensure("topology.rssi column",
    ["tables-db", "get-column", ...topology, "--key", "rssi"],
    ["tables-db", "create-integer-column", ...topology, "--key", "rssi", "--min", "-127", "--max", "0"]);
  ensure("topology.active column",
    ["tables-db", "get-column", ...topology, "--key", "active"],
    ["tables-db", "create-boolean-column", ...topology, "--key", "active", "--required", "true"]);
  ensure("topology gateway-peer index",
    ["tables-db", "get-index", ...topology, "--key", "gateway-peer"],
    ["tables-db", "create-index", ...topology, "--key", "gateway-peer", "--type", "unique", "--columns", "gatewayDeviceId", "peerDeviceId"]);
  ensure("topology gateway index",
    ["tables-db", "get-index", ...topology, "--key", "gateway-id"],
    ["tables-db", "create-index", ...topology, "--key", "gateway-id", "--type", "key", "--columns", "gatewayDeviceId"]);
  ensure("topology farm active index",
    ["tables-db", "get-index", ...topology, "--key", "farm-active"],
    ["tables-db", "create-index", ...topology, "--key", "farm-active", "--type", "key", "--columns", "farmId", "active"]);
  ensure("topology active reported index",
    ["tables-db", "get-index", ...topology, "--key", "active-reported"],
    ["tables-db", "create-index", ...topology, "--key", "active-reported", "--type", "key", "--columns", "active", "reportedAt", "--orders", "ASC", "DESC"]);

  const geofenceAreas = ["--database-id", config.databaseId, "--table-id", config.geofenceAreaTableId];
  ensure("geofence areas table",
    ["tables-db", "get-table", ...geofenceAreas],
    ["tables-db", "create-table", ...geofenceAreas, "--name", "Geofence areas", "--permissions", 'create("users")', "--row-security", "true"]);
  run(["tables-db", "update-table", ...geofenceAreas, "--permissions", 'create("users")', "--row-security", "true"]);
  for (const [key, size] of [["farmId", "36"], ["name", "128"], ["shape", "12"], ["geometry", "4000"]]) {
    ensure(`geofence areas.${key} column`,
      ["tables-db", "get-column", ...geofenceAreas, "--key", key],
      ["tables-db", "create-string-column", ...geofenceAreas, "--key", key, "--size", size, "--required", "true"]);
  }
  ensure("geofence areas farm index",
    ["tables-db", "get-index", ...geofenceAreas, "--key", "farm-id"],
    ["tables-db", "create-index", ...geofenceAreas, "--key", "farm-id", "--type", "key", "--columns", "farmId"]);

  const geofenceRules = ["--database-id", config.databaseId, "--table-id", config.geofenceRuleTableId];
  ensure("geofence rules table",
    ["tables-db", "get-table", ...geofenceRules],
    ["tables-db", "create-table", ...geofenceRules, "--name", "Geofence rules", "--permissions", 'create("users")', "--row-security", "true"]);
  run(["tables-db", "update-table", ...geofenceRules, "--permissions", 'create("users")', "--row-security", "true"]);
  for (const [key, size] of [["farmId", "36"], ["name", "128"], ["areaId", "36"]]) {
    ensure(`geofence rules.${key} column`,
      ["tables-db", "get-column", ...geofenceRules, "--key", key],
      ["tables-db", "create-string-column", ...geofenceRules, "--key", key, "--size", size, "--required", "true"]);
  }
  ensure("geofence rules.deviceIds column",
    ["tables-db", "get-column", ...geofenceRules, "--key", "deviceIds"],
    ["tables-db", "create-string-column", ...geofenceRules, "--key", "deviceIds", "--size", "36", "--array"]);
  for (const key of ["enterAlert", "exitAlert"]) {
    ensure(`geofence rules.${key} column`,
      ["tables-db", "get-column", ...geofenceRules, "--key", key],
      ["tables-db", "create-boolean-column", ...geofenceRules, "--key", key, "--required", "true"]);
  }
  ensure("geofence rules farm index",
    ["tables-db", "get-index", ...geofenceRules, "--key", "farm-id"],
    ["tables-db", "create-index", ...geofenceRules, "--key", "farm-id", "--type", "key", "--columns", "farmId"]);

  const alarms = ["--database-id", config.databaseId, "--table-id", config.geofenceAlarmTableId];
  ensure("geofence alarms table",
    ["tables-db", "get-table", ...alarms],
    ["tables-db", "create-table", ...alarms, "--name", "Geofence alarms", "--row-security", "true"]);
  run(["tables-db", "update-table", ...alarms, "--row-security", "true"]);
  for (const [key, size] of [["farmId", "36"], ["areaId", "36"], ["ruleId", "36"], ["deviceId", "36"], ["event", "8"], ["lastLocation", "128"], ["raisedAt", "40"]]) {
    ensure(`geofence alarms.${key} column`,
      ["tables-db", "get-column", ...alarms, "--key", key],
      ["tables-db", "create-string-column", ...alarms, "--key", key, "--size", size, "--required", "true"]);
  }
  for (const key of ["active", "acknowledged"]) {
    ensure(`geofence alarms.${key} column`,
      ["tables-db", "get-column", ...alarms, "--key", key],
      ["tables-db", "create-boolean-column", ...alarms, "--key", key, "--required", "true"]);
  }
  for (const key of ["clearedAt", "acknowledgedAt"]) {
    ensure(`geofence alarms.${key} column`,
      ["tables-db", "get-column", ...alarms, "--key", key],
      ["tables-db", "create-string-column", ...alarms, "--key", key, "--size", "40"]);
  }
  ensure("geofence alarms farm index",
    ["tables-db", "get-index", ...alarms, "--key", "farm-active"],
    ["tables-db", "create-index", ...alarms, "--key", "farm-active", "--type", "key", "--columns", "farmId", "active"]);
  ensure("geofence alarms rule device event index",
    ["tables-db", "get-index", ...alarms, "--key", "rule-device-event"],
    ["tables-db", "create-index", ...alarms, "--key", "rule-device-event", "--type", "unique", "--columns", "ruleId", "deviceId", "event"]);
}
