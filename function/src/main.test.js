import assert from "node:assert/strict";
import { after, beforeEach, test } from "node:test";
import { TablesDB } from "node-appwrite";

process.env.APPWRITE_FUNCTION_API_ENDPOINT = "https://appwrite.example/v1";
process.env.APPWRITE_FUNCTION_PROJECT_ID = "project-a";
process.env.LIVE_STOCKING_DATABASE_ID = "database-a";
process.env.LIVE_STOCKING_TELEMETRY_TABLE_ID = "telemetry-a";
process.env.LIVE_STOCKING_TOPOLOGY_TABLE_ID = "topology-a";
process.env.LIVE_STOCKING_OTA_UPDATE_TABLE_ID = "ota-a";
process.env.LIVE_STOCKING_DOWNLINK_TABLE_ID = "downlink-a";
process.env.LIVE_STOCKING_REPOSITORY_URL = "https://github.example/acme/live-stocking";
const { default: main } = await import("./main.js");

const gatewayId = "11111111-1111-4111-8111-111111111111";
const remoteId = "22222222-2222-4222-8222-222222222222";
const devices = new Map([
  [gatewayId, { $id: gatewayId, serial: "AABBCCDDEEFF", metadata: { farmId: "farm-a", icon: "gateway", markerColor: "blue" },
    $permissions: ['read("team:farm-a")'] }],
  [remoteId, { $id: remoteId, serial: "112233445566", metadata: { farmId: "farm-a", icon: "tracker", markerColor: "orange" },
    $permissions: ['read("team:farm-a")'] }],
]);
const originalFetch = globalThis.fetch;
const originalCreateRow = TablesDB.prototype.createRow;
const originalListRows = TablesDB.prototype.listRows;
const originalUpdateRow = TablesDB.prototype.updateRow;
const rows = [];
const topologyRows = [];
const otaRows = [];
const downlinkRows = [];
const timeseriesWrites = [];

beforeEach(() => {
  rows.length = 0;
  topologyRows.length = 0;
  otaRows.length = 0;
  downlinkRows.length = 0;
  timeseriesWrites.length = 0;
  process.env.APPWRITE_FUNCTION_API_ENDPOINT = "https://appwrite.example/v1";
  process.env.APPWRITE_FUNCTION_PROJECT_ID = "project-a";
  process.env.LIVE_STOCKING_DATABASE_ID = "database-a";
  process.env.LIVE_STOCKING_TELEMETRY_TABLE_ID = "telemetry-a";
  process.env.LIVE_STOCKING_TOPOLOGY_TABLE_ID = "topology-a";
  process.env.LIVE_STOCKING_OTA_UPDATE_TABLE_ID = "ota-a";
  globalThis.fetch = async (url, options = {}) => {
    if (url === "https://github.example/acme/live-stocking/releases/latest") {
      return { ok: true, status: 200, url: "https://github.example/acme/live-stocking/releases/tag/v0.0.4" };
    }
    if (url === "https://appwrite.example/v1/timeseries/stores/project-a/points") {
      timeseriesWrites.push(JSON.parse(options.body));
      return { ok: true, status: 201 };
    }
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
    if (args.tableId === "downlink-a") {
      const row = { $id: `downlink-${downlinkRows.length + 1}`, ...args.data };
      downlinkRows.push(row);
      return row;
    }
    const row = { ...args, $id: args.rowId };
    rows.push(row);
    return row;
  };
  TablesDB.prototype.listRows = async (args) => ({
    rows: args.tableId === "topology-a"
      ? topologyRows.filter((row) => row.gatewayDeviceId === gatewayId)
      : args.tableId === "ota-a"
        ? otaRows
        : args.tableId === "downlink-a" ? downlinkRows : [],
  });
  TablesDB.prototype.updateRow = async (args) => {
    if (args.tableId === "telemetry-a") {
      const telemetry = rows.find((candidate) => candidate.$id === args.rowId);
      if (!telemetry) throw Object.assign(new Error("not found"), { code: 404 });
      Object.assign(telemetry.data, args.data);
      telemetry.permissions = args.permissions;
      return telemetry;
    }
    const row = topologyRows.find((candidate) => candidate.$id === args.rowId);
    if (row) Object.assign(row, args.data);
    const ota = otaRows.find((candidate) => candidate.$id === args.rowId);
    if (ota) Object.assign(ota, args.data);
    const downlink = downlinkRows.find((candidate) => candidate.$id === args.rowId);
    if (downlink) Object.assign(downlink, args.data);
    return row || ota || downlink;
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
  assert.equal(rows[0].data.gatewayDeviceId, null);
  assert.equal(rows[1].data.gatewayDeviceId, gatewayId);
  assert.equal(JSON.parse(rows[1].data.payload).sensors[1].value, 59.3);
  assert.deepEqual(rows[1].data.location, [18, 59.3]);
  assert.deepEqual({ icon: rows[1].data.icon, markerColor: rows[1].data.markerColor },
    { icon: "tracker", markerColor: "orange" });
  assert.deepEqual(rows[1].permissions, devices.get(remoteId).$permissions);
  assert.equal(timeseriesWrites.length, 1);
  assert.match(timeseriesWrites[0].data, new RegExp(`device_${remoteId}.*sensor_battery_voltage=3\\.7`));
  assert.match(timeseriesWrites[0].data, /lat=59\.3,lon=18/);
  assert.doesNotMatch(timeseriesWrites[0].data, /sensor_(3|4)=/);
  assert.doesNotMatch(timeseriesWrites[0].data, /sensor_gps=/);
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

test("multiple queued readings remain in time series while TablesDB keeps one latest row per device", async () => {
  const result = await publish([
    { clientId: gatewayId, status: "online" },
    { clientId: remoteId, sensors: [{ type: 12, value: 3.7 }] },
    { clientId: remoteId, sensors: [{ type: 12, value: 3.65 }] },
  ]);
  assert.equal(result.status, 201);
  assert.equal(rows.length, 2);
  assert.deepEqual(rows.map(({ data }) => data.deviceId), [gatewayId, remoteId]);
  assert.equal(JSON.parse(rows[1].data.payload).sensors[0].value, 3.65);
  assert.match(timeseriesWrites[0].data, /sensor_battery_voltage=3\.7/);
  assert.match(timeseriesWrites[0].data, /sensor_battery_voltage=3\.65/);
});

test("legacy single-device telemetry remains accepted", async () => {
  const result = await publish({ status: "online", sensors: [{ type: 12, value: 3.8 }] });
  assert.equal(result.status, 201);
  assert.equal(rows.length, 1);
  assert.equal(rows[0].data.deviceId, gatewayId);
  assert.equal(rows[0].data.gatewayDeviceId, null);
});

test("sensor indexes become named fields and GPS remains numeric", async () => {
  const result = await publish({ sensors: [
    { type: 1, value: 21.5 }, { type: 2, value: 61 },
    { type: 3, value: 59.3293 }, { type: 4, value: 18.0686 },
    { type: 5, value: 2.4 },
    { type: 6, value: 0.1 }, { type: 7, value: 0.2 }, { type: 8, value: 9.8 },
    { type: 9, value: 0.01 }, { type: 10, value: 0.02 }, { type: 11, value: 0.03 },
    { type: 12, value: 3.8 },
  ] });
  assert.equal(result.status, 201);
  const line = timeseriesWrites[0].data;
  for (const field of ["temperature", "humidity", "length", "accel_x", "accel_y", "accel_z",
    "gyro_x", "gyro_y", "gyro_z", "battery_voltage"]) {
    assert.match(line, new RegExp(`sensor_${field}=`));
  }
  assert.match(line, /lat=59\.3293,lon=18\.0686/);
  assert.doesNotMatch(line, /sensor_\d+=|sensor_gps=/);
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
  const accepted = await publish({ clientId: gatewayId, firmwareVersion: "v0.0.3", ota: { requestId, status: "pending", detail: "accepted" } });
  assert.equal(accepted.status, 201);
  assert.equal(otaRows.length, 1);
  assert.deepEqual(otaRows[0].status, "pending");
  assert.equal(otaRows[0].targetFirmwareVersion, "v0.0.4");

  const completed = await publish({ clientId: gatewayId, firmwareVersion: "v0.0.4", ota: { requestId, status: "succeeded", detail: "installed" } });
  assert.equal(completed.status, 201);
  assert.equal(otaRows.length, 1);
  assert.equal(otaRows[0].status, "succeeded");
  assert.ok(otaRows[0].completedAt);
});

test("ordinary telemetry completes a pending OTA when the target version is running", async () => {
  const requestId = "44444444-4444-4444-8444-444444444444";
  await publish({ clientId: gatewayId, firmwareVersion: "v0.0.3", ota: { requestId, status: "pending", detail: "downloading" } });
  assert.equal(otaRows[0].status, "pending");

  const reconciled = await publish({ clientId: gatewayId, firmwareVersion: "0.0.4", status: "online" });
  assert.equal(reconciled.status, 201);
  assert.equal(otaRows[0].status, "succeeded");
  assert.equal(otaRows[0].detail, "running target firmware");
  assert.equal(otaRows[0].firmwareVersion, "0.0.4");
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

test("relayed devices report topology under their own device identity", async () => {
  const result = await publish([{ clientId: remoteId, topology: { links: [{
    peerId: gatewayId, peerRadioMac: "02:11:22:33:44:55", rssi: -61, ageMs: 500,
  }] } }]);
  assert.equal(result.status, 201);
  assert.equal(topologyRows.length, 1);
  assert.equal(topologyRows[0].gatewayDeviceId, remoteId);
  assert.equal(topologyRows[0].peerDeviceId, gatewayId);
});

test("a topology report rejects duplicate peers", async () => {
  const link = { peerId: remoteId, peerRadioMac: "02:11:22:33:44:55", ageMs: 0 };
  const result = await publish({ clientId: gatewayId, topology: { links: [link, link] } });
  assert.equal(result.status, 400);
  assert.equal(topologyRows.length, 0);
});

test("gateway downlink status is stored against the remote nRF device", async () => {
  let result = await publish({ clientId: remoteId, downlink: {
    requestId: "6ab551810011772b0ac9", revision: 7, status: "cached",
  } });
  assert.equal(result.status, 201);
  assert.equal(downlinkRows.length, 1);
  assert.equal(downlinkRows[0].deviceId, remoteId);
  assert.equal(downlinkRows[0].gatewayDeviceId, gatewayId);

  result = await publish({ clientId: remoteId, downlink: {
    requestId: "6ab551810011772b0ac9", revision: 7, status: "applied",
  } });
  assert.equal(result.status, 201);
  assert.equal(downlinkRows.length, 1);
  assert.equal(downlinkRows[0].status, "applied");
  assert.ok(downlinkRows[0].completedAt);
});
