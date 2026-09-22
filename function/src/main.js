import { Client, ID, TablesDB } from "node-appwrite";

const DATABASE_ID = process.env.LIVE_STOCKING_DATABASE_ID || process.env.DATABASE_ID;
const TELEMETRY_TABLE_ID = process.env.LIVE_STOCKING_TELEMETRY_TABLE_ID || process.env.TELEMETRY_TABLE_ID;

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
    if ((route.channel === "status" || route.channel === "battery") &&
        entry.batteryVoltageMv !== undefined &&
        (!Number.isInteger(entry.batteryVoltageMv) || entry.batteryVoltageMv < 2500 ||
         entry.batteryVoltageMv > 5000 || entry.unit !== "millivolt")) {
      return json(res, { error: "Battery telemetry must contain a valid batteryVoltageMv and millivolt unit" }, 400);
    }
    if (entry.latitude !== undefined || entry.longitude !== undefined) {
      if (typeof entry.latitude !== "number" || !Number.isFinite(entry.latitude) ||
          entry.latitude < -90 || entry.latitude > 90 ||
          typeof entry.longitude !== "number" || !Number.isFinite(entry.longitude) ||
          entry.longitude < -180 || entry.longitude > 180) {
        return json(res, { error: "Telemetry location must contain valid latitude and longitude" }, 400);
      }
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
      targets.push({ entry, target, readPermissions });
    }
    const receivedAt = new Date().toISOString();
    const telemetryIds = [];
    for (const { entry, target, readPermissions } of targets) {
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
      telemetryIds.push(row.$id);
    }
    return json(res, { accepted: true, telemetryId: telemetryIds[0], telemetryIds, receivedAt }, 201);
  } catch (caught) {
    error(caught instanceof Error ? caught.message : String(caught));
    return json(res, { error: "Could not persist MQTT telemetry" }, 500);
  }
}
