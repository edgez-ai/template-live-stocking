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
}
