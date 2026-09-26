import { createHash } from "node:crypto";
import { readFile, stat, writeFile } from "node:fs/promises";

const [bundlePath, outputPath, releaseTag, updateId, runtimeVersion] = process.argv.slice(2);
if (![bundlePath, outputPath, releaseTag, updateId, runtimeVersion].every(Boolean)) {
  throw new Error("Usage: create-app-bundle-manifest.mjs <bundle> <output> <release-tag> <update-id> <runtime-version>");
}
if (!/^[A-Za-z0-9_.-]+$/.test(releaseTag)) throw new Error("Unsafe release tag");
if (!/^[A-Za-z0-9_.-]+$/.test(runtimeVersion)) throw new Error("Unsafe runtime version");

const bytes = await readFile(bundlePath);
const details = await stat(bundlePath);
const manifest = {
  schemaVersion: 1,
  platform: "android",
  updateId,
  runtimeVersion,
  releaseTag,
  bundleAssetName: "live-stocking.android.bundle",
  sha256: createHash("sha256").update(bytes).digest("hex"),
  size: details.size,
  createdAt: new Date().toISOString(),
};
await writeFile(outputPath, JSON.stringify(manifest));
