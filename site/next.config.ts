import type { NextConfig } from "next";

function publicClientEndpoint(value: string | undefined) {
  if (!value) return value;
  const endpoint = new URL(value);
  const localHosts = new Set(["localhost", "127.0.0.1", "::1"]);
  if (endpoint.protocol === "http:" && !localHosts.has(endpoint.hostname)) endpoint.protocol = "https:";
  return endpoint.toString().replace(/\/$/, "");
}

const required = {
  NEXT_PUBLIC_APPWRITE_ENDPOINT: publicClientEndpoint(
    process.env.IOT_PROV_ENDPOINT || process.env.APPWRITE_PUBLIC_ENDPOINT || process.env.APPWRITE_ENDPOINT,
  ),
  NEXT_PUBLIC_APPWRITE_PROJECT_ID:
    process.env.IOT_PROV_PROJECT_ID ||
    process.env.APPWRITE_PROJECT_ID ||
    process.env.APPWRITE_SITE_PROJECT_ID,
  NEXT_PUBLIC_DATABASE_ID: process.env.IOT_PROV_DATABASE_ID || process.env.DATABASE_ID,
  NEXT_PUBLIC_TELEMETRY_TABLE_ID: process.env.IOT_PROV_TELEMETRY_TABLE_ID || process.env.TELEMETRY_TABLE_ID,
};

const missing = Object.entries(required).filter(([, value]) => !value).map(([key]) => key);
if (missing.length) throw new Error(`Missing public Appwrite configuration: ${missing.join(", ")}`);

const nextConfig: NextConfig = { env: required, output: "export" };
export default nextConfig;
