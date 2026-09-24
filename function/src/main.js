import { Client, ID, Query, TablesDB } from "node-appwrite";

const DATABASE_ID = process.env.LIVE_STOCKING_DATABASE_ID || process.env.DATABASE_ID;
const TELEMETRY_TABLE_ID = process.env.LIVE_STOCKING_TELEMETRY_TABLE_ID || process.env.TELEMETRY_TABLE_ID;
const TOPOLOGY_TABLE_ID = process.env.LIVE_STOCKING_TOPOLOGY_TABLE_ID || "topology-links";
const OTA_UPDATE_TABLE_ID = process.env.LIVE_STOCKING_OTA_UPDATE_TABLE_ID || "ota-updates";
const GEOFENCE_AREA_TABLE_ID = process.env.LIVE_STOCKING_GEOFENCE_AREA_TABLE_ID;
const GEOFENCE_RULE_TABLE_ID = process.env.LIVE_STOCKING_GEOFENCE_RULE_TABLE_ID;
const GEOFENCE_ALARM_TABLE_ID = process.env.LIVE_STOCKING_GEOFENCE_ALARM_TABLE_ID;
const SENSOR_BATTERY_VOLTAGE = 12;
const SENSOR_LATITUDE = 3;
const SENSOR_LONGITUDE = 4;
const SENSOR_ACCELEROMETER = [6, 7, 8];
const SENSOR_GYROSCOPE = [9, 10, 11];
const SENSOR_TYPE_MAX = 12;
const OTA_STATUSES = new Set(["pending", "succeeded", "failed", "busy"]);

function sensorValue(payload, type) {
  if (!Array.isArray(payload?.sensors)) return null;
  const sensor = payload.sensors.find((candidate) => candidate?.type === type);
  return typeof sensor?.value === "number" && Number.isFinite(sensor.value) ? sensor.value : null;
}

function validTopology(topology) {
  const valid = topology && typeof topology === "object" && !Array.isArray(topology) &&
    Array.isArray(topology.links) && topology.links.length <= 16 &&
    topology.links.every((link) => link && typeof link === "object" &&
      typeof link.peerId === "string" && /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(link.peerId) &&
      typeof link.peerRadioMac === "string" && /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(link.peerRadioMac) &&
      Number.isInteger(link.ageMs) && link.ageMs >= 0 && link.ageMs <= 300000 &&
      (link.rssi === undefined || (Number.isInteger(link.rssi) && link.rssi >= -127 && link.rssi <= 0)));
  return Boolean(valid) && new Set(topology.links.map((link) => link.peerId)).size === topology.links.length;
}

async function syncTopology(tables, gateway, links, peerDevices, readPermissions, reportedAt) {
  const existing = await tables.listRows({
    databaseId: DATABASE_ID,
    tableId: TOPOLOGY_TABLE_ID,
    queries: [Query.equal("gatewayDeviceId", gateway.$id), Query.limit(100)],
  });
  const rowsByPeer = new Map((existing.rows || []).map((row) => [row.peerDeviceId, row]));
  const activePeerIds = new Set();
  for (let index = 0; index < links.length; index += 1) {
    const link = links[index];
    const peer = peerDevices[index];
    activePeerIds.add(peer.$id);
    const data = {
      farmId: gateway.metadata.farmId,
      gatewayDeviceId: gateway.$id,
      gatewaySerial: gateway.serial,
      peerDeviceId: peer.$id,
      peerSerial: peer.serial,
      peerRadioMac: link.peerRadioMac.toUpperCase(),
      rssi: link.rssi ?? null,
      active: true,
      lastSeenAt: new Date(new Date(reportedAt).getTime() - link.ageMs).toISOString(),
      reportedAt,
    };
    const row = rowsByPeer.get(peer.$id);
    if (row) {
      await tables.updateRow({ databaseId: DATABASE_ID, tableId: TOPOLOGY_TABLE_ID, rowId: row.$id, data, permissions: readPermissions });
    } else {
      await tables.createRow({ databaseId: DATABASE_ID, tableId: TOPOLOGY_TABLE_ID, rowId: ID.unique(), data, permissions: readPermissions });
    }
  }
  for (const row of existing.rows || []) {
    if (!activePeerIds.has(row.peerDeviceId) && row.active) {
      await tables.updateRow({
        databaseId: DATABASE_ID,
        tableId: TOPOLOGY_TABLE_ID,
        rowId: row.$id,
        data: { active: false, reportedAt },
        permissions: readPermissions,
      });
    }
  }
}

function otaStatus(entry) {
  const ota = entry?.ota;
  if (!ota || typeof ota !== "object" || Array.isArray(ota)) return null;
  if (typeof ota.requestId !== "string" || !ota.requestId || ota.requestId.length > 64 ||
      typeof ota.status !== "string" || !OTA_STATUSES.has(ota.status) ||
      (ota.detail !== undefined && (typeof ota.detail !== "string" || ota.detail.length > 256))) return undefined;
  return { requestId: ota.requestId, status: ota.status, detail: ota.detail || "" };
}

async function syncOtaUpdate(tables, target, entry, readPermissions, reportedAt) {
  const update = otaStatus(entry);
  if (!update) return;
  const existing = await tables.listRows({
    databaseId: DATABASE_ID,
    tableId: OTA_UPDATE_TABLE_ID,
    queries: [Query.equal("requestId", update.requestId), Query.limit(1)],
  });
  const firmwareVersion = typeof entry.firmwareVersion === "string" && entry.firmwareVersion.length <= 128
    ? entry.firmwareVersion : "";
  const completed = update.status === "succeeded" || update.status === "failed" || update.status === "busy";
  const data = {
    deviceId: target.$id,
    serial: target.serial,
    requestId: update.requestId,
    status: update.status,
    detail: update.detail,
    firmwareVersion,
    reportedAt,
    completedAt: completed ? reportedAt : null,
  };
  const row = existing.rows?.[0];
  if (row) {
    await tables.updateRow({ databaseId: DATABASE_ID, tableId: OTA_UPDATE_TABLE_ID, rowId: row.$id, data, permissions: readPermissions });
  } else {
    await tables.createRow({ databaseId: DATABASE_ID, tableId: OTA_UPDATE_TABLE_ID, rowId: ID.unique(), data, permissions: readPermissions });
  }
}

function locationOf(payload) {
  const latitude = sensorValue(payload, SENSOR_LATITUDE);
  const longitude = sensorValue(payload, SENSOR_LONGITUDE);
  return latitude !== null && longitude !== null && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180
    ? { latitude, longitude } : null;
}

function localMeters(point, center, rotation = 0) {
  const latitude = (point.latitude - center.latitude) * 111320;
  const longitude = (point.longitude - center.longitude) * 111320 * Math.cos(center.latitude * Math.PI / 180);
  const radians = -rotation * Math.PI / 180;
  return { x: longitude * Math.cos(radians) - latitude * Math.sin(radians), y: longitude * Math.sin(radians) + latitude * Math.cos(radians) };
}

function containsArea(area, point) {
  let geometry;
  try { geometry = JSON.parse(area.geometry); } catch { return false; }
  const center = geometry.center;
  if (!center || !Number.isFinite(center.latitude) || !Number.isFinite(center.longitude)) return false;
  if (area.shape === "polygon") {
    const vertices = geometry.vertices;
    if (!Array.isArray(vertices) || vertices.length < 3) return false;
    let inside = false;
    for (let i = 0, j = vertices.length - 1; i < vertices.length; j = i++) {
      const a = vertices[i], b = vertices[j];
      if (((a.latitude > point.latitude) !== (b.latitude > point.latitude)) &&
          point.longitude < (b.longitude - a.longitude) * (point.latitude - a.latitude) /
            (b.latitude - a.latitude) + a.longitude) inside = !inside;
    }
    return inside;
  }
  const { x, y } = localMeters(point, center, geometry.rotationDegrees || 0);
  if (area.shape === "circle") return x * x + y * y <= geometry.radiusMeters ** 2;
  if (area.shape === "oval") return (x / geometry.radiusXMeters) ** 2 + (y / geometry.radiusYMeters) ** 2 <= 1;
  if (area.shape === "rectangle") return Math.abs(x) <= geometry.widthMeters / 2 && Math.abs(y) <= geometry.heightMeters / 2;
  return false;
}

async function evaluateGeofences(tables, target, entry, readPermissions, receivedAt) {
  if (!GEOFENCE_AREA_TABLE_ID || !GEOFENCE_RULE_TABLE_ID || !GEOFENCE_ALARM_TABLE_ID) return;
  const point = locationOf(entry);
  const farmId = target.metadata?.farmId;
  if (!point || !farmId) return;
  const [areasResult, rulesResult, alarmsResult] = await Promise.all([
    tables.listRows({ databaseId: DATABASE_ID, tableId: GEOFENCE_AREA_TABLE_ID, queries: [Query.equal("farmId", farmId), Query.limit(100)] }),
    tables.listRows({ databaseId: DATABASE_ID, tableId: GEOFENCE_RULE_TABLE_ID, queries: [Query.equal("farmId", farmId), Query.limit(100)] }),
    tables.listRows({ databaseId: DATABASE_ID, tableId: GEOFENCE_ALARM_TABLE_ID, queries: [Query.equal("deviceId", target.$id), Query.limit(100)] }),
  ]);
  const areas = new Map((areasResult.rows || []).map((area) => [area.$id, area]));
  const alarms = new Map((alarmsResult.rows || []).map((alarm) => [`${alarm.ruleId}:${alarm.event}`, alarm]));
  for (const rule of rulesResult.rows || []) {
    const area = areas.get(rule.areaId);
    if (!area || (Array.isArray(rule.deviceIds) && rule.deviceIds.length && !rule.deviceIds.includes(target.$id))) continue;
    const isInside = containsArea(area, point);
    for (const [event, matches] of [["enter", Boolean(rule.enterAlert) && isInside], ["exit", Boolean(rule.exitAlert) && !isInside]]) {
      const existing = alarms.get(`${rule.$id}:${event}`);
      const location = `${point.latitude.toFixed(6)},${point.longitude.toFixed(6)}`;
      if (matches && !existing) {
        await tables.createRow({ databaseId: DATABASE_ID, tableId: GEOFENCE_ALARM_TABLE_ID, rowId: ID.unique(), data: {
          farmId, areaId: area.$id, ruleId: rule.$id, deviceId: target.$id, event, active: true, acknowledged: false,
          lastLocation: location, raisedAt: receivedAt,
        }, permissions: readPermissions });
      } else if (existing && existing.active !== matches) {
        await tables.updateRow({ databaseId: DATABASE_ID, tableId: GEOFENCE_ALARM_TABLE_ID, rowId: existing.$id, data: {
          active: matches, lastLocation: location, ...(matches ? { acknowledged: false, raisedAt: receivedAt, clearedAt: "" } : { clearedAt: receivedAt }),
        } });
      }
    }
  }
}

function json(res, payload, status = 200) {
  return res.json(payload, status, { "cache-control": "no-store" });
}

function bodyOf(req) {
  if (req.bodyJson && typeof req.bodyJson === "object") return req.bodyJson;
  if (!req.body) return {};
  try { return JSON.parse(req.body); } catch { return {}; }
}

function eventDeviceId(req) {
  const event = req.headers["x-appwrite-event"] || "";
  return /^devices\.(.+)\.mqtt\.message\.publish$/.exec(event)?.[1] || "";
}

function topicParts(topic) {
  const match = /^projects\/([^/]+)\/devices\/([A-Za-z0-9][A-Za-z0-9._:-]{0,35})\/telemetry\/(.+)$/.exec(topic);
  return match ? { projectId: match[1], serial: match[2], channel: match[3] } : null;
}

async function getDevice(req, deviceId) {
  const endpoint = process.env.APPWRITE_FUNCTION_API_ENDPOINT.replace(/\/+$/, "");
  const response = await fetch(`${endpoint}/devices/${encodeURIComponent(deviceId)}`, {
    headers: {
      "x-appwrite-project": process.env.APPWRITE_FUNCTION_PROJECT_ID,
      "x-appwrite-key": req.headers["x-appwrite-key"],
    },
  });
  if (response.status === 404) return null;
  if (!response.ok) throw new Error(`Device lookup failed with ${response.status}`);
  return response.json();
}

export default async function main({ req, res, error }) {
  if (!DATABASE_ID || !TELEMETRY_TABLE_ID) {
    return json(res, { error: "Function environment is incomplete" }, 500);
  }

  const body = bodyOf(req);
  const deviceId = eventDeviceId(req);
  if (!deviceId || body.event !== "message.publish") {
    return json(res, { accepted: false, reason: "not_mqtt_publish_event" }, 202);
  }

  const topic = typeof body.topic === "string" ? body.topic.trim() : "";
  const route = topicParts(topic);
  if (!route || route.projectId !== process.env.APPWRITE_FUNCTION_PROJECT_ID) {
    return json(res, { error: "MQTT topic does not match this Appwrite project" }, 400);
  }

  let payload;
  try {
    payload = typeof body.payload === "string" ? JSON.parse(body.payload) : body.payload;
  } catch {
    return json(res, { error: "MQTT payload must be valid JSON" }, 400);
  }
  const entries = Array.isArray(payload) ? payload : [payload];
  if (!entries.length || entries.length > 16 || entries.some((entry) =>
      !entry || typeof entry !== "object" || Array.isArray(entry))) {
    return json(res, { error: "MQTT payload must contain 1 to 16 telemetry objects" }, 400);
  }
  if (JSON.stringify(payload).length > 10000) {
    return json(res, { error: "MQTT payload is too large" }, 413);
  }
  for (const entry of entries) {
    if (Array.isArray(payload) &&
        (typeof entry.clientId !== "string" ||
         !/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(entry.clientId))) {
      return json(res, { error: "Each telemetry entry needs a device clientId" }, 400);
    }
    if (entry.clientId !== undefined && typeof entry.clientId !== "string") {
      return json(res, { error: "Telemetry clientId must be a string" }, 400);
    }
    if (entry.sensors !== undefined && (!Array.isArray(entry.sensors) || entry.sensors.length > 12 || entry.sensors.some((sensor) =>
        !sensor || typeof sensor !== "object" || !Number.isInteger(sensor.type) || sensor.type < 1 || sensor.type > SENSOR_TYPE_MAX ||
        typeof sensor.value !== "number" || !Number.isFinite(sensor.value)))) {
      return json(res, { error: "Telemetry sensors must contain supported numeric type and value fields" }, 400);
    }
    if (entry.topology !== undefined && !validTopology(entry.topology)) {
      return json(res, { error: "Topology must contain at most 16 valid direct peer links" }, 400);
    }
    if (route.channel === "ota" && !otaStatus(entry)) {
      return json(res, { error: "OTA telemetry must contain a valid OTA status" }, 400);
    }
    const sensorTypes = new Set((entry.sensors || []).map((sensor) => sensor.type));
    for (const [name, axes] of [["accelerometer", SENSOR_ACCELEROMETER], ["gyroscope", SENSOR_GYROSCOPE]]) {
      if (axes.some((type) => sensorTypes.has(type)) && !axes.every((type) => sensorTypes.has(type))) {
        return json(res, { error: `IMU ${name} data must include X, Y, and Z axes` }, 400);
      }
    }
    const batteryVoltage = sensorValue(entry, SENSOR_BATTERY_VOLTAGE);
    if ((route.channel === "status" || route.channel === "battery") && batteryVoltage !== null &&
        (batteryVoltage < 2.5 || batteryVoltage > 5.0)) {
      return json(res, { error: "Battery sensor voltage must be between 2.5 and 5.0 volts" }, 400);
    }
    const latitude = sensorValue(entry, SENSOR_LATITUDE);
    const longitude = sensorValue(entry, SENSOR_LONGITUDE);
    if ((latitude === null) !== (longitude === null) ||
        (latitude !== null && (latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180))) {
      return json(res, { error: "Location sensors must include valid latitude and longitude" }, 400);
    }
  }

  const client = new Client()
    .setEndpoint(process.env.APPWRITE_FUNCTION_API_ENDPOINT)
    .setProject(process.env.APPWRITE_FUNCTION_PROJECT_ID)
    .setKey(req.headers["x-appwrite-key"]);
  const tables = new TablesDB(client);

  try {
    const device = await getDevice(req, deviceId);
    if (!device || device.serial !== route.serial) {
      return json(res, { error: "MQTT topic serial does not match the Appwrite device" }, 403);
    }
    const targets = [];
    for (const entry of entries) {
      const targetId = entry.clientId || deviceId;
      const target = targetId === deviceId ? device : await getDevice(req, targetId);
      if (!target || target.$id !== targetId) {
        return json(res, { error: "Telemetry clientId does not match an Appwrite device" }, 403);
      }
      if (targetId !== deviceId &&
          (!device.metadata?.farmId || target.metadata?.farmId !== device.metadata.farmId)) {
        return json(res, { error: "Remote beacon is not in the gateway's farm" }, 403);
      }
      const readPermissions = (target.$permissions || []).filter((permission) => permission.startsWith("read("));
      if (!readPermissions.length) {
        return json(res, { error: "Appwrite device has no owner read permission" }, 409);
      }
      if (entry.topology && targetId !== deviceId) {
        return json(res, { error: "Only the publishing gateway can report topology" }, 403);
      }
      const topologyPeers = [];
      for (const link of entry.topology?.links || []) {
        if (link.peerId === deviceId) return json(res, { error: "A topology link cannot target its gateway" }, 400);
        const peer = await getDevice(req, link.peerId);
        if (!peer || peer.$id !== link.peerId || !device.metadata?.farmId || peer.metadata?.farmId !== device.metadata.farmId) {
          return json(res, { error: "Topology peer is not an Appwrite device in the gateway farm" }, 403);
        }
        topologyPeers.push(peer);
      }
      targets.push({ entry, target, readPermissions, topologyPeers });
    }
    const receivedAt = new Date().toISOString();
    const telemetryIds = [];
    for (const { entry, target, readPermissions, topologyPeers } of targets) {
      const row = await tables.createRow({
        databaseId: DATABASE_ID,
        tableId: TELEMETRY_TABLE_ID,
        rowId: ID.unique(),
        data: {
          deviceId: target.$id,
          serial: target.serial,
          channel: route.channel,
          topic,
          payload: JSON.stringify(entry),
          receivedAt,
        },
        permissions: readPermissions,
      });
      await evaluateGeofences(tables, target, entry, readPermissions, receivedAt);
      await syncOtaUpdate(tables, target, entry, readPermissions, receivedAt);
      if (entry.topology) {
        await syncTopology(tables, device, entry.topology.links, topologyPeers, readPermissions, receivedAt);
      }
      telemetryIds.push(row.$id);
    }
    return json(res, { accepted: true, telemetryId: telemetryIds[0], telemetryIds, receivedAt }, 201);
  } catch (caught) {
    error(caught instanceof Error ? caught.message : String(caught));
    return json(res, { error: "Could not persist MQTT telemetry" }, 500);
  }
}
