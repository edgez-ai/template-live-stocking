const { existsSync } = require("node:fs");
const path = require("node:path");
const { loadEnvFile } = require("node:process");

const rootEnv = path.resolve(__dirname, "..", ".env.local");
if (existsSync(rootEnv)) loadEnvFile(rootEnv);

const appName = process.env.APP_NAME;
const domainSuffix = process.env.DOMAIN_SUFFIX;
const configuredEndpoint = process.env.APPWRITE_PUBLIC_ENDPOINT || process.env.APPWRITE_ENDPOINT;
const projectId = process.env.APPWRITE_PROJECT_ID;
const databaseId = process.env.DATABASE_ID;
const telemetryTableId = process.env.TELEMETRY_TABLE_ID;

if (!appName || !domainSuffix || !configuredEndpoint || !projectId || !databaseId || !telemetryTableId) {
  throw new Error("Appwrite project and telemetry table environment is incomplete");
}

const endpointUrl = new URL(configuredEndpoint);
const localHosts = new Set(["localhost", "127.0.0.1", "::1"]);
if (endpointUrl.protocol === "http:" && !localHosts.has(endpointUrl.hostname)) endpointUrl.protocol = "https:";
const endpoint = endpointUrl.toString().replace(/\/$/, "");

const bundlePrefix = domainSuffix.split(".").reverse().join(".");
const androidName = appName.replace(/[^A-Za-z0-9_]/g, "_").replace(/^[^A-Za-z_]+/, "app");
const platform = `${bundlePrefix}.${androidName}`;

module.exports = {
  expo: {
    name: appName,
    slug: appName,
    scheme: "edgez-devtools",
    version: "1.0.0",
    orientation: "portrait",
    userInterfaceStyle: "light",
    plugins: [["@orbital-systems/react-native-esp-idf-provisioning", { transport: "ble" }]],
    android: { package: platform },
    extra: {
      appwriteEndpoint: endpoint,
      appwriteProjectId: projectId,
      appwritePlatform: platform,
      databaseId,
      telemetryTableId,
    },
  },
};
