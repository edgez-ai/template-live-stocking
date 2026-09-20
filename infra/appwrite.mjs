import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";
import { loadEnvFile } from "node:process";
import { fileURLToPath } from "node:url";

export const infraDir = path.dirname(fileURLToPath(import.meta.url));
export const rootDir = path.resolve(infraDir, "..");
const envFile = path.join(rootDir, ".env.local");
if (existsSync(envFile)) loadEnvFile(envFile);

const cli = path.join(infraDir, "node_modules", ".bin", "appwrite");
export const dryRun = process.env.INFRA_DRY_RUN === "1";
if (!existsSync(cli)) {
  console.error("Run npm install in infra/ first.");
  process.exit(1);
}

const required = [
  "APP_NAME", "DOMAIN_SUFFIX", "APPWRITE_ENDPOINT", "APPWRITE_PROJECT_ID",
  "APPWRITE_PROJECT_NAME", "APPWRITE_API_KEY", "DATABASE_ID", "TELEMETRY_TABLE_ID",
];
const missing = required.filter((key) => !process.env[key]);
if (missing.length) {
  console.error(`Missing environment variables: ${missing.join(", ")}`);
  process.exit(1);
}

function publicClientEndpoint(value) {
  const endpoint = new URL(value);
  const localHosts = new Set(["localhost", "127.0.0.1", "::1"]);
  if (endpoint.protocol === "http:" && !localHosts.has(endpoint.hostname)) endpoint.protocol = "https:";
  return endpoint.toString().replace(/\/$/, "");
}

export const config = {
  name: process.env.APP_NAME,
  domainSuffix: process.env.DOMAIN_SUFFIX,
  endpoint: process.env.APPWRITE_ENDPOINT,
  publicEndpoint: publicClientEndpoint(process.env.APPWRITE_PUBLIC_ENDPOINT || process.env.APPWRITE_ENDPOINT),
  projectId: process.env.APPWRITE_PROJECT_ID,
  projectName: process.env.APPWRITE_PROJECT_NAME,
  apiKey: process.env.APPWRITE_API_KEY,
  databaseId: process.env.DATABASE_ID,
  telemetryTableId: process.env.TELEMETRY_TABLE_ID,
  pollIntervalMs: process.env.POLL_INTERVAL_MS || "5000",
};
export const domainPrefix = `${config.projectName}-${config.name}`;
export const webDomain = `${domainPrefix}.sites.${config.domainSuffix}`;
export const functionDomain = `${domainPrefix}.functions.${config.domainSuffix}`;
export const androidApplicationId = `${config.domainSuffix.split(".").reverse().join(".")}.${config.name.replace(/[^A-Za-z0-9_]/g, "_").replace(/^[^A-Za-z_]+/, "app")}`;

if (!/^[a-z][a-z0-9-]*$/.test(config.name)) throw new Error("APP_NAME must be DNS-safe");
if (domainPrefix.length > 63) throw new Error("Public domain prefix exceeds 63 characters");

function printable(args) {
  return ["appwrite", ...args]
    .map((value) => value === config.apiKey ? "<redacted>" : value)
    .join(" ");
}

export function run(args, options = {}) {
  if (dryRun) {
    console.log(`[dry-run] ${printable(args)}`);
    return { status: options.probe ? 1 : 0, stdout: "" };
  }
  const result = spawnSync(cli, args, {
    cwd: options.cwd || infraDir,
    encoding: "utf8",
    stdio: options.capture ? "pipe" : "inherit",
  });
  if (!options.allowFailure && result.status !== 0) {
    if (options.capture) process.stderr.write(result.stderr || result.stdout || "");
    throw new Error(`Command failed: ${printable(args)}`);
  }
  return result;
}

export function exists(args) {
  return run(args, { capture: true, allowFailure: true, probe: true }).status === 0;
}

export function ensure(label, probeArgs, createArgs) {
  if (exists(probeArgs)) return console.log(`Kept existing ${label}`);
  run(createArgs);
  console.log(`Created ${label}`);
}

export function upsertResourceVariable(group, resourceFlag, resourceId, key, value, secret = false) {
  const secretValue = secret ? "true" : "false";
  if (dryRun) {
    return run([group, "create-variable", resourceFlag, resourceId, "--key", key, "--value", value, "--secret", secretValue]);
  }
  const result = run([group, "list-variables", resourceFlag, resourceId, "--json"], { capture: true });
  const current = (JSON.parse(result.stdout).variables || []).find((item) => item.key === key);
  if (current) {
    run([group, "update-variable", resourceFlag, resourceId, "--variable-id", current.$id, "--key", key, "--value", value, "--secret", secretValue]);
  } else {
    run([group, "create-variable", resourceFlag, resourceId, "--key", key, "--value", value, "--secret", secretValue]);
  }
}

export function ensureProxyDomain(type, resourceId, domain) {
  const found = !dryRun && run(["proxy", "list-rules", "--where", `domain=${domain}`, "--limit", "1", "--json"], { capture: true }).stdout.includes(domain);
  if (found) return console.log(`Kept existing ${type} domain ${domain}`);
  run(type === "Function"
    ? ["proxy", "create-function-rule", "--domain", domain, "--function-id", resourceId]
    : ["proxy", "create-site-rule", "--domain", domain, "--site-id", resourceId]);
}

export function configureClient() {
  run(["client", "--endpoint", config.endpoint, "--project-id", config.projectId, "--key", config.apiKey]);
}
