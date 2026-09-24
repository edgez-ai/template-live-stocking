import assert from "node:assert/strict";
import { after, beforeEach, test } from "node:test";
import { TablesDB } from "node-appwrite";

process.env.APPWRITE_FUNCTION_API_ENDPOINT = "https://appwrite.example/v1";
process.env.APPWRITE_FUNCTION_PROJECT_ID = "project-a";
process.env.LIVE_STOCKING_DATABASE_ID = "database-a";
process.env.LIVE_STOCKING_TELEMETRY_TABLE_ID = "telemetry-a";
process.env.LIVE_STOCKING_TOPOLOGY_TABLE_ID = "topology-a";
process.env.LIVE_STOCKING_OTA_UPDATE_TABLE_ID = "ota-a";
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
const originalListRows = TablesDB.prototype.listRows;
const originalUpdateRow = TablesDB.prototype.updateRow;
const rows = [];
const topologyRows = [];
const otaRows = [];

beforeEach(() => {
  rows.length = 0;
  topologyRows.length = 0;
  otaRows.length = 0;
  process.env.APPWRITE_FUNCTION_API_ENDPOINT = "https://appwrite.example/v1";
  process.env.APPWRITE_FUNCTION_PROJECT_ID = "project-a";
  process.env.LIVE_STOCKING_DATABASE_ID = "database-a";
  process.env.LIVE_STOCKING_TELEMETRY_TABLE_ID = "telemetry-a";
  process.env.LIVE_STOCKING_TOPOLOGY_TABLE_ID = "topology-a";
  process.env.LIVE_STOCKING_OTA_UPDATE_TABLE_ID = "ota-a";
  globalThis.fetch = async (url) => {
    const device = devices.get(decodeURIComponent(url.split("/").pop()));
    return { ok: Boolean(device), status: device ? 200 : 404, json: async () => device };
  };
  TablesDB.prototype.createRow = async (args) => {
    if (args.tableId === "topology-a") {
      const row = { $id: `topology-${topologyRows.length + 1}`, ...args.data };
      topologyRows.push(row);
      return row;
    }
    if (args.tableId === "ota-a") {
      const row = { $id: `ota-${otaRows.length + 1}`, ...args.data };
      otaRows.push(row);
      return row;
    }
    rows.push(args);
    return { $id: `telemetry-${rows.length}` };
  };
  TablesDB.prototype.listRows = async (args) => ({
    rows: args.tableId === "topology-a"
      ? topologyRows.filter((row) => row.gatewayDeviceId === gatewayId)
      : args.tableId === "ota-a"
        ? otaRows : [],
  });
  TablesDB.prototype.updateRow = async (args) => {
    const row = topologyRows.find((candidate) => candidate.$id === args.rowId);
    if (row) Object.assign(row, args.data);
    const ota = otaRows.find((candidate) => candidate.$id === args.rowId);
    if (ota) Object.assign(ota, args.data);
    return row || ota;
  };
});

after(() => {
  globalThis.fetch = originalFetch;
  TablesDB.prototype.createRow = originalCreateRow;
  TablesDB.prototype.listRows = originalListRows;
  TablesDB.prototype.updateRow = originalUpdateRow;
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
    { clientId: gatewayId, sensors: [{ type: 12, value: 3.9 }] },
    { clientId: remoteId, sensors: [{ type: 12, value: 3.7 }, { type: 3, value: 59.3 }, { type: 4, value: 18.0 }] },
  ]);
  assert.equal(result.status, 201);
  assert.equal(rows.length, 2);
  assert.deepEqual(rows.map(({ data }) => [data.deviceId, data.serial]),
    [[gatewayId, "AABBCCDDEEFF"], [remoteId, "112233445566"]]);
  assert.equal(JSON.parse(rows[1].data.payload).sensors[1].value, 59.3);
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
    { clientId: remoteId, sensors: [{ type: 12, value: 3.7 }] },
    { clientId: remoteId, sensors: [{ type: 12, value: 3.65 }] },
  ]);
  assert.equal(result.status, 201);
  assert.equal(rows.length, 3);
  assert.deepEqual(rows.map(({ data }) => data.deviceId), [gatewayId, remoteId, remoteId]);
  assert.deepEqual(rows.slice(1).map(({ data }) => JSON.parse(data.payload).sensors[0].value),
    [3.7, 3.65]);
});

test("legacy single-device telemetry remains accepted", async () => {
  const result = await publish({ status: "online", sensors: [{ type: 12, value: 3.8 }] });
  assert.equal(result.status, 201);
  assert.equal(rows.length, 1);
  assert.equal(rows[0].data.deviceId, gatewayId);
});


test("complete IMU readings are stored with unified telemetry", async () => {
  const result = await publish({ status: "online", sensors: [
    { type: 6, value: 0.12 }, { type: 7, value: -0.34 }, { type: 8, value: 9.81 },
    { type: 9, value: 0.01 }, { type: 10, value: 0.02 }, { type: 11, value: -0.03 },
  ] });
  assert.equal(result.status, 201);
  assert.deepEqual(JSON.parse(rows[0].data.payload).sensors.map((sensor) => sensor.type), [6, 7, 8, 9, 10, 11]);
});

test("partial IMU readings are rejected", async () => {
  const result = await publish({ status: "online", sensors: [{ type: 6, value: 0.12 }] });
  assert.equal(result.status, 400);
  assert.match(result.body.error, /IMU accelerometer/);
  assert.equal(rows.length, 0);
});

test("OTA telemetry creates and completes one status row", async () => {
  const requestId = "33333333-3333-4333-8333-333333333333";
  const accepted = await publish({ clientId: gatewayId, firmwareVersion: "v0.0.4", ota: { requestId, status: "pending", detail: "accepted" } });
  assert.equal(accepted.status, 201);
  assert.equal(otaRows.length, 1);
  assert.deepEqual(otaRows[0].status, "pending");

  const completed = await publish({ clientId: gatewayId, firmwareVersion: "v0.0.4", ota: { requestId, status: "succeeded", detail: "installed" } });
  assert.equal(completed.status, 201);
  assert.equal(otaRows.length, 1);
  assert.equal(otaRows[0].status, "succeeded");
  assert.ok(otaRows[0].completedAt);
});

test("gateway topology upserts direct links and marks missing peers inactive", async () => {
  const link = { peerId: remoteId, peerRadioMac: "02:11:22:33:44:55", rssi: -67, ageMs: 1200 };
  let result = await publish({ clientId: gatewayId, status: "online", topology: { links: [link] } });
  assert.equal(result.status, 201);
  assert.equal(topologyRows.length, 1);
  assert.deepEqual(
    { gatewayDeviceId: topologyRows[0].gatewayDeviceId, peerDeviceId: topologyRows[0].peerDeviceId, rssi: topologyRows[0].rssi, active: topologyRows[0].active },
    { gatewayDeviceId: gatewayId, peerDeviceId: remoteId, rssi: -67, active: true },
  );

  result = await publish({ clientId: gatewayId, status: "online", topology: { links: [] } });
  assert.equal(result.status, 201);
  assert.equal(topologyRows.length, 1);
  assert.equal(topologyRows[0].active, false);
});

test("remote devices cannot report gateway topology", async () => {
  const result = await publish([{ clientId: remoteId, topology: { links: [] } }]);
  assert.equal(result.status, 403);
  assert.equal(topologyRows.length, 0);
});

test("a topology report rejects duplicate peers", async () => {
  const link = { peerId: remoteId, peerRadioMac: "02:11:22:33:44:55", ageMs: 0 };
  const result = await publish({ clientId: gatewayId, topology: { links: [link, link] } });
  assert.equal(result.status, 400);
  assert.equal(topologyRows.length, 0);
});
