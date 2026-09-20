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
    ["IOT_PROV_APP_NAME", config.name],
    ["IOT_PROV_PROJECT_NAME", config.projectName],
    ["IOT_PROV_DOMAIN_SUFFIX", config.domainSuffix],
    ["IOT_PROV_ENDPOINT", config.publicEndpoint],
    ["IOT_PROV_PROJECT_ID", config.projectId],
    ["IOT_PROV_DATABASE_ID", config.databaseId],
    ["IOT_PROV_TELEMETRY_TABLE_ID", config.telemetryTableId],
  ];
  for (const [key, value] of variables) upsertResourceVariable("sites", "--site-id", id, key, value);
  run(["sites", "create-deployment", "--site-id", id, "--code", "../site", "--install-command", "npm install", "--build-command", "npm run build", "--output-directory", "out", "--activate", "true"], { cwd: infraDir });
  ensureProxyDomain("Site", id, webDomain);
}
