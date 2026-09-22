import assert from "node:assert/strict";
import { after, beforeEach, test } from "node:test";
import { TablesDB } from "node-appwrite";

process.env.APPWRITE_FUNCTION_API_ENDPOINT = "https://appwrite.example/v1";
process.env.APPWRITE_FUNCTION_PROJECT_ID = "project-a";
process.env.LIVE_STOCKING_DATABASE_ID = "database-a";
process.env.LIVE_STOCKING_TELEMETRY_TABLE_ID = "telemetry-a";
const { default: main } = await import("./main.js");

const gatewayId = "11111111-1111-4111-8111-111111111111";
const remoteId = "22222222-2222-4222-8222-222222222222";
const devices = new Map([
  [gatewayId, { $id: gatewayId, serial: "AABBCCDDEEFF", metadata: { farmId: "farm-a" },
    $permissions: ['read("team:farm-a")'] }],
  [remoteId, { $id: remoteId, serial: "112233445566", metadata: { farmId: "farm-a" },
    $permissions: ['read("team:farm-a")'] }],
]);
const originalFetch = globalThis.fetch;
const originalCreateRow = TablesDB.prototype.createRow;
const rows = [];

beforeEach(() => {
  rows.length = 0;
  process.env.APPWRITE_FUNCTION_API_ENDPOINT = "https://appwrite.example/v1";
  process.env.APPWRITE_FUNCTION_PROJECT_ID = "project-a";
  process.env.LIVE_STOCKING_DATABASE_ID = "database-a";
  process.env.LIVE_STOCKING_TELEMETRY_TABLE_ID = "telemetry-a";
  globalThis.fetch = async (url) => {
    const device = devices.get(decodeURIComponent(url.split("/").pop()));
    return { ok: Boolean(device), status: device ? 200 : 404, json: async () => device };
  };
  TablesDB.prototype.createRow = async (args) => {
    rows.push(args);
    return { $id: `telemetry-${rows.length}` };
  };
});

after(() => {
  globalThis.fetch = originalFetch;
  TablesDB.prototype.createRow = originalCreateRow;
});

async function publish(payload) {
  const req = {
    headers: {
      "x-appwrite-event": `devices.${gatewayId}.mqtt.message.publish`,
      "x-appwrite-key": "test-key",
    },
    bodyJson: {
      event: "message.publish",
      topic: "projects/project-a/devices/AABBCCDDEEFF/telemetry/status",
      payload: JSON.stringify(payload),
    },
  };
  return main({ req, res: { json: (body, status) => ({ body, status }) }, error: (message) => {
    throw new Error(message);
  } });
}

test("one MQTT batch saves each clientId under its own device and permissions", async () => {
  const result = await publish([
    { clientId: gatewayId, batteryVoltageMv: 3900, unit: "millivolt" },
    { clientId: remoteId, batteryVoltageMv: 3700, unit: "millivolt", latitude: 59.3, longitude: 18.0 },
  ]);
  assert.equal(result.status, 201);
  assert.equal(rows.length, 2);
  assert.deepEqual(rows.map(({ data }) => [data.deviceId, data.serial]),
    [[gatewayId, "AABBCCDDEEFF"], [remoteId, "112233445566"]]);
  assert.equal(JSON.parse(rows[1].data.payload).latitude, 59.3);
  assert.deepEqual(rows[1].permissions, devices.get(remoteId).$permissions);
});

test("a gateway cannot write a remote device from another farm", async () => {
  devices.get(remoteId).metadata.farmId = "farm-b";
  try {
    const result = await publish([{ clientId: gatewayId }, { clientId: remoteId }]);
    assert.equal(result.status, 403);
    assert.equal(rows.length, 0);
  } finally {
    devices.get(remoteId).metadata.farmId = "farm-a";
  }
});

test("multiple queued readings from one clientId remain separate rows", async () => {
  const result = await publish([
    { clientId: gatewayId, status: "online" },
    { clientId: remoteId, batteryVoltageMv: 3700, unit: "millivolt" },
    { clientId: remoteId, batteryVoltageMv: 3650, unit: "millivolt" },
  ]);
  assert.equal(result.status, 201);
  assert.equal(rows.length, 3);
  assert.deepEqual(rows.map(({ data }) => data.deviceId), [gatewayId, remoteId, remoteId]);
  assert.deepEqual(rows.slice(1).map(({ data }) => JSON.parse(data.payload).batteryVoltageMv),
    [3700, 3650]);
});

test("legacy single-device telemetry remains accepted", async () => {
  const result = await publish({ status: "online", batteryVoltageMv: 3800, unit: "millivolt" });
  assert.equal(result.status, 201);
  assert.equal(rows.length, 1);
  assert.equal(rows[0].data.deviceId, gatewayId);
});
