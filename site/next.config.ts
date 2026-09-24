import { existsSync, readFileSync } from "node:fs";
import path from "node:path";
import type { NextConfig } from "next";

function localResourceIds() {
  const configPath = path.resolve(process.cwd(), "..", "appwrite.config.json");
  if (!existsSync(configPath)) return {};
  const config = JSON.parse(readFileSync(configPath, "utf8"));
  const telemetryTable = config.tables?.find((table: { $id?: string }) => table.$id === "telemetry");
  const topologyTable = config.tables?.find((table: { $id?: string }) => table.$id === "topology-links");
  return {
    databaseId: telemetryTable?.databaseId as string | undefined,
    telemetryTableId: telemetryTable?.$id as string | undefined,
    topologyTableId: topologyTable?.$id as string | undefined,
  };
}

function publicClientEndpoint(value: string | undefined) {
  if (!value) return value;
  const endpoint = new URL(value);
  const localHosts = new Set(["localhost", "127.0.0.1", "::1"]);
  if (endpoint.protocol === "http:" && !localHosts.has(endpoint.hostname)) endpoint.protocol = "https:";
  return endpoint.toString().replace(/\/$/, "");
}

const localResources = localResourceIds();
const required = {
  NEXT_PUBLIC_APPWRITE_ENDPOINT: publicClientEndpoint(
    process.env.LIVE_STOCKING_ENDPOINT || process.env.APPWRITE_PUBLIC_ENDPOINT || process.env.APPWRITE_ENDPOINT,
  ),
  NEXT_PUBLIC_APPWRITE_PROJECT_ID:
    process.env.LIVE_STOCKING_PROJECT_ID ||
    process.env.APPWRITE_PROJECT_ID ||
    process.env.APPWRITE_SITE_PROJECT_ID,
  NEXT_PUBLIC_DATABASE_ID:
    process.env.LIVE_STOCKING_DATABASE_ID || localResources.databaseId,
  NEXT_PUBLIC_TELEMETRY_TABLE_ID:
    process.env.LIVE_STOCKING_TELEMETRY_TABLE_ID || localResources.telemetryTableId,
  NEXT_PUBLIC_TOPOLOGY_TABLE_ID:
    process.env.LIVE_STOCKING_TOPOLOGY_TABLE_ID || localResources.topologyTableId,
};

const missing = Object.entries(required).filter(([, value]) => !value).map(([key]) => key);
if (missing.length) throw new Error(`Missing public Appwrite configuration: ${missing.join(", ")}`);

const nextConfig: NextConfig = { env: required, output: "export" };
export default nextConfig;
