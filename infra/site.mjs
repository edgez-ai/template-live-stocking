import { config, ensureProxyDomain, exists, infraDir, run, upsertResourceVariable, webDomain } from "./appwrite.mjs";

export function installSite() {
  const id = `${config.name}-site`;
  const settings = [
    "--site-id", id, "--name", `${config.name} web`, "--framework", "nextjs",
    "--build-runtime", "node-22", "--enabled", "true", "--logging", "true",
    "--timeout", "30", "--install-command", "npm install", "--build-command", "npm run build",
    "--output-directory", "out", "--adapter", "static", "--fallback-file", "index.html",
  ];
  run(exists(["sites", "get", "--site-id", id]) ? ["sites", "update", ...settings] : ["sites", "create", ...settings]);
  const variables = [
    ["LIVE_STOCKING_APP_NAME", config.name],
    ["LIVE_STOCKING_PROJECT_NAME", config.projectName],
    ["LIVE_STOCKING_DOMAIN_SUFFIX", config.domainSuffix],
    ["LIVE_STOCKING_ENDPOINT", config.publicEndpoint],
    ["LIVE_STOCKING_PROJECT_ID", config.projectId],
    ["LIVE_STOCKING_DATABASE_ID", config.databaseId],
    ["LIVE_STOCKING_TELEMETRY_TABLE_ID", config.telemetryTableId],
    ["LIVE_STOCKING_TOPOLOGY_TABLE_ID", config.topologyTableId],
  ];
  for (const [key, value] of variables) upsertResourceVariable("sites", "--site-id", id, key, value);
  run(["sites", "create-deployment", "--site-id", id, "--code", "../site", "--install-command", "npm install", "--build-command", "npm run build", "--output-directory", "out", "--activate", "true"], { cwd: infraDir });
  ensureProxyDomain("Site", id, webDomain);
}
