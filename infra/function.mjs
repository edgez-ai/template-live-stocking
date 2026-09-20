import { config, exists, infraDir, run, upsertResourceVariable } from "./appwrite.mjs";

export function installFunction() {
  const id = config.name;
  const settings = [
    "--function-id", id, "--name", `${config.name} MQTT ingest`,
    "--runtime", "node-24", "--events", "devices.*.mqtt.message.publish", "--timeout", "15",
    "--enabled", "true", "--logging", "true", "--entrypoint", "src/main.js",
    "--commands", "npm install", "--scopes", "devices.read", "rows.write",
  ];
  run(exists(["functions", "get", "--function-id", id])
    ? ["functions", "update", ...settings]
    : ["functions", "create", ...settings]);
  upsertResourceVariable("functions", "--function-id", id, "IOT_PROV_DATABASE_ID", config.databaseId);
  upsertResourceVariable("functions", "--function-id", id, "IOT_PROV_TELEMETRY_TABLE_ID", config.telemetryTableId);
  run(["functions", "create-deployment", "--function-id", id, "--code", "../function", "--activate", "true", "--entrypoint", "src/main.js", "--commands", "npm install"], { cwd: infraDir });
}
