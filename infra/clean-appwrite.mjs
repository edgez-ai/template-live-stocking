import {
  config,
  configureClient,
  dryRun,
  exists,
  functionDomain,
  run,
  webDomain,
} from "./appwrite.mjs";

function removeIfPresent(label, probeArgs, deleteArgs) {
  if (dryRun) {
    run(deleteArgs);
    return;
  }
  if (!exists(probeArgs)) {
    console.log(`Skipped missing ${label}`);
    return;
  }
  run(deleteArgs);
  console.log(`Deleted ${label}`);
}

function removeProxyRules(domains) {
  if (dryRun) {
    for (const domain of domains) console.log(`[dry-run] delete proxy rule for ${domain}`);
    return;
  }
  const result = run(["proxy", "list-rules", "--limit", "100", "--json"], { capture: true });
  const rules = JSON.parse(result.stdout).rules || [];
  for (const rule of rules) {
    if (!domains.includes(rule.domain)) continue;
    run(["proxy", "delete-rule", "--rule-id", rule.$id]);
    console.log(`Deleted proxy rule ${rule.domain}`);
  }
}

configureClient();

console.log(`Cleaning Appwrite resources for ${config.name} in project ${config.projectId}`);
removeProxyRules([webDomain, functionDomain]);

removeIfPresent(
  "site",
  ["sites", "get", "--site-id", `${config.name}-site`],
  ["sites", "delete", "--site-id", `${config.name}-site`],
);
removeIfPresent(
  "MQTT ingest function",
  ["functions", "get", "--function-id", config.name],
  ["functions", "delete", "--function-id", config.name],
);
removeIfPresent(
  "telemetry database",
  ["tables-db", "get", "--database-id", config.databaseId],
  ["tables-db", "delete", "--database-id", config.databaseId],
);

for (const [label, id] of [
  ["web auth platform", `${config.name}-web`],
  ["local web auth platform", `${config.name}-local`],
  ["Android auth platform", `${config.name}-android`],
]) {
  removeIfPresent(
    label,
    ["project", "get-platform", "--platform-id", id],
    ["project", "delete-platform", "--platform-id", id],
  );
}

console.log("Cleanup complete. Project auth methods, users, and Devices were left unchanged.");
