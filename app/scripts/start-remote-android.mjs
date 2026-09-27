import { execFile, spawn } from "node:child_process";
import { createRequire } from "node:module";
import net from "node:net";
import { promisify } from "node:util";

const require = createRequire(import.meta.url);
const execFileAsync = promisify(execFile);
const inviteUrl = require("../app.config.js").expo.extra.teamInviteUrl;
const port = Number(process.env.EXPO_PORT ?? 8081);
const metroUrl = `http://127.0.0.1:${port}`;
const developmentClientUrl = `edgez-devtools://expo-development-client/?url=${encodeURIComponent(metroUrl)}`;
const nodeOptions = process.env.NODE_OPTIONS?.includes("--dns-result-order=")
  ? process.env.NODE_OPTIONS
  : [process.env.NODE_OPTIONS, "--dns-result-order=ipv4first"].filter(Boolean).join(" ");

const expo = spawn(
  "expo",
  ["start", "--dev-client", "--host", "localhost", "--port", String(port)],
  { stdio: "inherit", env: { ...process.env, NODE_OPTIONS: nodeOptions, EXPO_PUBLIC_APPWRITE_TEAM_INVITE_URL: inviteUrl } },
);

function stop() {
  if (!expo.killed) expo.kill("SIGTERM");
}

process.once("SIGINT", stop);
process.once("SIGTERM", stop);
process.once("exit", stop);

function isMetroReady() {
  return new Promise((resolve) => {
    const socket = net.createConnection({ host: "127.0.0.1", port });
    socket.once("connect", () => { socket.end(); resolve(true); });
    socket.once("error", () => resolve(false));
  });
}

const deadline = Date.now() + 30_000;
while (!(await isMetroReady())) {
  if (expo.exitCode !== null) process.exit(expo.exitCode ?? 1);
  if (Date.now() >= deadline) {
    console.error(`Timed out waiting for Metro on port ${port}.`);
    stop();
    process.exit(1);
  }
  await new Promise((resolve) => setTimeout(resolve, 250));
}

async function resolveAndroidSerial() {
  if (process.env.ANDROID_SERIAL) return process.env.ANDROID_SERIAL;

  const { stdout } = await execFileAsync("adb", ["devices"]);
  const devices = stdout
    .split("\n")
    .slice(1)
    .map((line) => line.trim().split(/\s+/))
    .filter(([, state]) => state === "device")
    .map(([deviceSerial]) => deviceSerial);

  if (devices.length === 0) throw new Error("No authorized Android device is connected.");

  // Prefer the direct wireless-debugging address when mDNS also exposes the
  // same phone as a second ADB transport.
  return devices.find((deviceSerial) => /^\d{1,3}(?:\.\d{1,3}){3}:\d+$/.test(deviceSerial)) ?? devices[0];
}

let serial;
try {
  serial = await resolveAndroidSerial();
  await execFileAsync("adb", ["-s", serial, "reverse", `tcp:${port}`, `tcp:${port}`]);
  console.log(`Forwarding Android tcp:${port} to Metro through ${serial}.`);
  await execFileAsync("adb", [
    "-s", serial,
    "shell", "am", "start",
    "-a", "android.intent.action.VIEW",
    "-d", developmentClientUrl,
  ]);
} catch (error) {
  console.error(`Failed to prepare the Android development client: ${error.message}`);
  stop();
  process.exit(1);
}

const expoExitCode = expo.exitCode !== null || expo.signalCode !== null
  ? expo.exitCode
  : await new Promise((resolve) => expo.once("exit", resolve));
process.exit(expoExitCode ?? 0);
