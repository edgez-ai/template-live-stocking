const appwriteConfig = require("../appwrite.config.json");
const edgezProject = require("../edgez.json");

const telemetryTable = appwriteConfig.tables?.find((table) => table.$id === "telemetry");
const databaseId = telemetryTable?.databaseId;
const telemetryTableId = telemetryTable?.$id;
const topologyTableId = appwriteConfig.tables?.find((table) => table.$id === "topology-links")?.$id;
const otaUpdateTableId = appwriteConfig.tables?.find((table) => table.$id === "ota-updates")?.$id;
const farmTableId = appwriteConfig.tables?.find((table) => table.$id === "farms")?.$id;
const geofenceAreaTableId = appwriteConfig.tables?.find((table) => table.$id === "geofence-areas")?.$id;
const geofenceRuleTableId = appwriteConfig.tables?.find((table) => table.$id === "geofence-rules")?.$id;
const geofenceAlarmTableId = appwriteConfig.tables?.find((table) => table.$id === "geofence-alarms")?.$id;
const appName = process.env.APP_NAME || edgezProject.name;
const domainSuffix = process.env.DOMAIN_SUFFIX;
const configuredEndpoint =
  process.env.APPWRITE_PUBLIC_ENDPOINT || process.env.APPWRITE_ENDPOINT || appwriteConfig.endpoint;
const projectId = process.env.APPWRITE_PROJECT_ID;

if (!appName || !domainSuffix || !configuredEndpoint || !projectId || !databaseId || !telemetryTableId || !topologyTableId || !otaUpdateTableId || !farmTableId || !geofenceAreaTableId || !geofenceRuleTableId || !geofenceAlarmTableId) {
  throw new Error("Appwrite project and telemetry table environment is incomplete");
}

const endpointUrl = new URL(configuredEndpoint);
const localHosts = new Set(["localhost", "127.0.0.1", "::1"]);
if (endpointUrl.protocol === "http:" && !localHosts.has(endpointUrl.hostname)) endpointUrl.protocol = "https:";
const endpoint = endpointUrl.toString().replace(/\/$/, "");

const bundlePrefix = domainSuffix.split(".").reverse().join(".");
const androidName = appName.replace(/[^A-Za-z0-9_]/g, "_").replace(/^[^A-Za-z_]+/, "app");
const platform = `${bundlePrefix}.${androidName}`;
const teamInviteUrl = process.env.APPWRITE_TEAM_INVITE_URL ||
  `https://${appName}-${projectId.slice(0, 8)}.sites.${domainSuffix}/invite.html`;
const vcsRepositoryUrl = process.env.LIVE_STOCKING_REPOSITORY_URL || process.env.APPWRITE_VCS_REPOSITORY_URL ||
  (process.env.GITHUB_REPOSITORY ? `${process.env.GITHUB_SERVER_URL || "https://github.com"}/${process.env.GITHUB_REPOSITORY}` : "");

module.exports = {
  expo: {
    name: appName,
    slug: appName,
    scheme: "edgez-devtools",
    version: "1.0.0",
    orientation: "portrait",
    userInterfaceStyle: "light",
    plugins: [
      "expo-status-bar",
      ["expo-location", { locationWhenInUsePermission: "Use your current location when placing a device on the farm map." }],
      ["@orbital-systems/react-native-esp-idf-provisioning", { transport: "ble" }],
      "react-native-ble-plx",
      "./plugins/withOrganicMaps",
      "./plugins/withUnsignedRelease",
    ],
    android: { package: platform },
    extra: {
      appwriteEndpoint: endpoint,
      appwriteProjectId: projectId,
      appwritePlatform: platform,
      teamInviteUrl,
      databaseId,
      telemetryTableId,
      topologyTableId,
      otaUpdateTableId,
      farmTableId,
      geofenceAreaTableId,
      geofenceRuleTableId,
      geofenceAlarmTableId,
      otaRepositoryUrl: vcsRepositoryUrl.replace(/\.git$/, "").replace(/\/$/, ""),
    },
  },
};
