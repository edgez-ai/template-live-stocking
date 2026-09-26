import { readFile, writeFile } from "node:fs/promises";

const [payloadPath, signaturePath, outputPath] = process.argv.slice(2);
if (![payloadPath, signaturePath, outputPath].every(Boolean)) {
  throw new Error("Usage: wrap-app-bundle-manifest.mjs <payload> <signature> <output>");
}
const envelope = {
  schemaVersion: 1,
  signedPayload: (await readFile(payloadPath)).toString("base64"),
  signature: (await readFile(signaturePath)).toString("base64"),
};
await writeFile(outputPath, `${JSON.stringify(envelope, null, 2)}\n`);
