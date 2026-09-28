"use client";

import { Account, Client, ID, Models, Query, TablesDB } from "appwrite";
import { useCallback, useEffect, useMemo, useState } from "react";

type Device = { $id: string; serial: string; name: string; status: string; enabled: boolean; metadata?: { firmwareTarget?: string; [key: string]: unknown } };
type Telemetry = Models.Row & { deviceId: string; gatewayDeviceId?: string | null; serial: string; channel: string; topic: string; payload: string; location?: [number, number] | null; icon?: string | null; markerColor?: string | null; receivedAt: string };
type TopologyLink = Models.Row & { farmId: string; gatewayDeviceId: string; gatewaySerial: string; peerDeviceId: string; peerSerial: string; peerRadioMac: string; rssi?: number | null; active: boolean; lastSeenAt: string; reportedAt: string };
type OtaUpdate = Models.Row & { deviceId: string; serial: string; requestId: string; status: "pending" | "succeeded" | "failed" | "busy"; detail?: string; firmwareVersion?: string; targetFirmwareVersion?: string; reportedAt: string; completedAt?: string | null };
type HistoryRange = "30m" | "1h" | "6h" | "24h";
type SensorPoint = { timestamp: number; value: number };
type SensorSeries = { field: string; label: string; color: string; points: SensorPoint[] };
type TimeseriesRow = { _time?: unknown; _field?: unknown; _value?: unknown; channel?: unknown };
type HistoryReading = { timestamp: number; channel: string; payload: string };
type SensorPayload = {
  batteryVoltageMv?: unknown;
  sensors?: { type?: unknown; value?: unknown }[];
};

const client = new Client()
  .setEndpoint(process.env.NEXT_PUBLIC_APPWRITE_ENDPOINT!)
  .setProject(process.env.NEXT_PUBLIC_APPWRITE_PROJECT_ID!);
const account = new Account(client);
const tables = new TablesDB(client);
const databaseId = process.env.NEXT_PUBLIC_DATABASE_ID!;
const telemetryTableId = process.env.NEXT_PUBLIC_TELEMETRY_TABLE_ID!;
const topologyTableId = process.env.NEXT_PUBLIC_TOPOLOGY_TABLE_ID!;
const otaUpdateTableId = process.env.NEXT_PUBLIC_OTA_UPDATE_TABLE_ID!;
const endpoint = process.env.NEXT_PUBLIC_APPWRITE_ENDPOINT!.replace(/\/+$/, "");
const projectId = process.env.NEXT_PUBLIC_APPWRITE_PROJECT_ID!;
const otaRepositoryUrl = (process.env.NEXT_PUBLIC_OTA_REPOSITORY_URL || "").replace(/\.git$/, "").replace(/\/$/, "");
const otaRepositoryMatch = /^https:\/\/github\.com\/([^/]+)\/([^/]+)$/.exec(otaRepositoryUrl);
const otaImageUrl = otaRepositoryMatch ? `${otaRepositoryUrl}/releases/latest/download/live-stocking-ota.bin` : "";
const otaLatestReleaseApiUrl = otaRepositoryMatch
  ? `https://api.github.com/repos/${otaRepositoryMatch[1]}/${otaRepositoryMatch[2]}/releases/latest` : "";
const historyRanges: { key: HistoryRange; label: string; duration: number }[] = [
  { key: "30m", label: "30 min", duration: 30 * 60 * 1000 },
  { key: "1h", label: "1 hour", duration: 60 * 60 * 1000 },
  { key: "6h", label: "6 hours", duration: 6 * 60 * 60 * 1000 },
  { key: "24h", label: "24 hours", duration: 24 * 60 * 60 * 1000 },
];
const topologyRecentMs = 2 * 60 * 1000;
const chartColors = ["#0a8c87", "#d47a1f", "#7c5ce5", "#d14b78", "#3977c3", "#6c8f2d", "#a75b32", "#53646f"];

function measurementForDevice(deviceId: string) {
  return `device_${deviceId.replace(/[^A-Za-z0-9_-]/g, "_")}`;
}

function timeseriesRange(duration: number) {
  if (duration <= 60 * 60 * 1000) return "1h";
  if (duration <= 6 * 60 * 60 * 1000) return "6h";
  return "24h";
}

async function queryDeviceTimeseries(deviceId: string, duration: number) {
  const jwt = (await account.createJWT()).jwt;
  const response = await fetch(`${endpoint}/timeseries/stores/${encodeURIComponent(projectId)}/queries`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "x-appwrite-project": projectId,
      "x-appwrite-jwt": jwt,
    },
    body: JSON.stringify({
      measurement: measurementForDevice(deviceId),
      range: timeseriesRange(duration),
      limit: 1000,
    }),
  });
  const payload = await response.json() as { rows?: TimeseriesRow[]; message?: string };
  if (!response.ok) throw new Error(payload.message || "Could not query time-series data.");
  return payload.rows || [];
}

function sensorLabel(field: string) {
  if (!field.startsWith("sensor_")) return field;
  const name = field.slice("sensor_".length).replaceAll("_", " ");
  return name.replace(/\b\w/g, (letter) => letter.toUpperCase());
}

function timeseriesSensorSeries(rows: TimeseriesRow[], duration: number): SensorSeries[] {
  const since = Date.now() - duration;
  const pointsByField = new Map<string, SensorPoint[]>();
  for (const row of rows) {
    if (typeof row._field !== "string" || !row._field.startsWith("sensor_")) continue;
    const timestamp = typeof row._time === "string" ? Date.parse(row._time) : NaN;
    const value = typeof row._value === "number" ? row._value : Number(row._value);
    if (!Number.isFinite(timestamp) || timestamp < since || !Number.isFinite(value)) continue;
    const points = pointsByField.get(row._field) || [];
    points.push({ timestamp, value });
    pointsByField.set(row._field, points);
  }
  return [...pointsByField.entries()]
    .sort(([left], [right]) => left.localeCompare(right, undefined, { numeric: true }))
    .map(([field, points], index) => ({
      field,
      label: sensorLabel(field),
      color: chartColors[index % chartColors.length],
      points: points.sort((left, right) => left.timestamp - right.timestamp),
    }));
}

function timeseriesReadings(rows: TimeseriesRow[]): HistoryReading[] {
  return rows.flatMap((row) => {
    if (row._field !== "payload" || typeof row._value !== "string") return [];
    const timestamp = typeof row._time === "string" ? Date.parse(row._time) : NaN;
    if (!Number.isFinite(timestamp)) return [];
    return [{ timestamp, channel: typeof row.channel === "string" ? row.channel : "telemetry", payload: row._value }];
  }).sort((left, right) => right.timestamp - left.timestamp).slice(0, 10);
}

function errorMessage(error: unknown) {
  return error instanceof Error ? error.message : "Something went wrong.";
}

const batteryVoltageSensorType = 12;
const temperatureSensorType = 1;
const randomTemperatureIntervalSeconds = 30;
const randomTemperatureScript = `math.randomseed((os.time() % 100000) + math.floor((os.clock() or 0) * 1000))

local value = math.random(180, 320) / 10

return {
  {
    type = 1,
    float_value = value,
  },
}`;

function voltageOf(row: Telemetry) {
  try {
    if (row.channel !== "status" && row.channel !== "battery") return null;
    const payload = JSON.parse(row.payload) as SensorPayload;
    const sensorValue = payload.sensors?.find((sensor) => sensor?.type === batteryVoltageSensorType)?.value;
    if (typeof sensorValue === "number" && Number.isFinite(sensorValue) && sensorValue >= 2.5 && sensorValue <= 5.0) {
      return sensorValue;
    }
    const legacyMillivolts = Number(payload.batteryVoltageMv);
    return Number.isInteger(legacyMillivolts) && legacyMillivolts >= 2500 && legacyMillivolts <= 5000
      ? legacyMillivolts / 1000 : null;
  } catch { return null; }
}

function temperatureOf(row: Telemetry) {
  try {
    const payload = JSON.parse(row.payload) as SensorPayload;
    const value = payload.sensors?.find((sensor) => sensor?.type === temperatureSensorType)?.value;
    return typeof value === "number" && Number.isFinite(value) ? value : null;
  } catch { return null; }
}

function firmwareVersionOf(row?: Telemetry) {
  if (!row) return "";
  try {
    const value = (JSON.parse(row.payload) as { firmwareVersion?: unknown }).firmwareVersion;
    return typeof value === "string" ? value : "";
  } catch { return ""; }
}

function sameFirmwareVersion(running: string, latest: string) {
  const normalize = (value: string) => value.trim().replace(/^v/i, "");
  return Boolean(running && latest) && normalize(running) === normalize(latest);
}

function statusOf(device: Device, latest?: Telemetry) {
  if (!device.enabled) return "Disabled";
  if (!latest) return "No data";
  return Date.now() - new Date(latest.receivedAt).getTime() <= 2 * 60 * 1000 ? "Online" : "Offline";
}

function relativeTime(value: string) {
  const seconds = Math.max(0, Math.round((Date.now() - new Date(value).getTime()) / 1000));
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  return `${Math.floor(seconds / 3600)}h ago`;
}

function isRecentTopology(link: TopologyLink) {
  return link.active && Date.now() - new Date(link.reportedAt).getTime() <= topologyRecentMs;
}

function mqttGatewayFor(device: Device, devices: Device[], topology: TopologyLink[]) {
  if (device.metadata?.mqttGateway === true || topology.some((link) => isRecentTopology(link) && link.gatewayDeviceId === device.$id)) return undefined;
  const link = topology.find((candidate) => isRecentTopology(candidate) && candidate.peerDeviceId === device.$id);
  if (link) return link.gatewayDeviceId;
  const farmId = device.metadata?.farmId;
  return devices.find((candidate) => candidate.metadata?.mqttGateway === true && candidate.metadata?.farmId === farmId)?.$id;
}

function prettyPayload(payload: string) {
  try { return JSON.stringify(JSON.parse(payload), null, 2); }
  catch { return payload; }
}

function SensorChart({ series, duration }: { series: SensorSeries[]; duration: number }) {
  const width = 720;
  const height = 260;
  const inset = 28;
  if (!series.length) return <div className="chart-empty">Select a sensor with data in this range.</div>;
  const values = series.flatMap((item) => item.points.map((point) => point.value));
  const rawMin = Math.min(...values);
  const rawMax = Math.max(...values);
  const padding = Math.max(0.05, (rawMax - rawMin) * .15);
  const min = rawMin - padding;
  const max = rawMax + padding;
  const end = Date.now();
  const start = end - duration;

  return <div className="chart-wrap">
    <svg className="chart" viewBox={`0 0 ${width} ${height}`} role="img" aria-label="Selected sensor history line chart">
      {[0, 1, 2, 3].map((line) => <line key={line} className="chart-grid" x1={inset} x2={width - inset} y1={inset + line * (height - inset * 2) / 3} y2={inset + line * (height - inset * 2) / 3} />)}
      {series.map((item) => {
        const sampled = item.points.length <= 240 ? item.points : item.points.filter((_, index) => index % Math.ceil(item.points.length / 240) === 0 || index === item.points.length - 1);
        const coordinates = sampled.map((point) => ({
          x: inset + Math.max(0, Math.min(1, (point.timestamp - start) / duration)) * (width - inset * 2),
          y: inset + (1 - (point.value - min) / (max - min)) * (height - inset * 2),
        }));
        const last = coordinates[coordinates.length - 1];
        return <g key={item.field}>
          <polyline className="chart-line" style={{ stroke: item.color }} points={coordinates.map((point) => `${point.x},${point.y}`).join(" ")} />
          {last && <circle className="chart-dot" style={{ fill: item.color }} cx={last.x} cy={last.y} r="5" />}
        </g>;
      })}
      <text className="chart-label" x={width - 5} y={15} textAnchor="end">{rawMax.toFixed(2)}</text>
      <text className="chart-label" x={width - 5} y={height - 5} textAnchor="end">{rawMin.toFixed(2)}</text>
    </svg>
  </div>;
}

function TopologyGraph({ device, links }: { device: Device; links: TopologyLink[] }) {
  const neighbors = links.map((link) => ({
    id: link.gatewayDeviceId === device.$id ? link.peerDeviceId : link.gatewayDeviceId,
    serial: link.gatewayDeviceId === device.$id ? link.peerSerial : link.gatewaySerial,
    rssi: link.rssi,
    lastSeenAt: link.lastSeenAt,
  })).filter((neighbor, index, items) => items.findIndex((item) => item.id === neighbor.id) === index);
  const height = Math.max(220, neighbors.length * 74);
  const centerY = height / 2;
  if (!neighbors.length) return <div className="topology-empty">No active mesh links reported for this device.</div>;
  return <div className="topology-wrap"><svg className="topology-graph" viewBox={`0 0 720 ${height}`} role="img" aria-label={`Mesh topology for ${device.name}`}>
    {neighbors.map((neighbor, index) => {
      const y = neighbors.length === 1
        ? centerY
        : 40 + index * ((height - 80) / (neighbors.length - 1));
      return <g key={neighbor.id}>
        <line className="topology-edge" x1="360" y1={centerY} x2="590" y2={y} />
        <text className="topology-signal" x="475" y={(centerY + y) / 2 - 7} textAnchor="middle">{typeof neighbor.rssi === "number" ? `${neighbor.rssi} dBm` : "direct"}</text>
        <circle className="topology-peer" cx="590" cy={y} r="25" />
        <text className="topology-node-label" x="625" y={y - 3}>{neighbor.serial}</text>
        <text className="topology-node-meta" x="625" y={y + 13}>{relativeTime(neighbor.lastSeenAt)}</text>
      </g>;
    })}
    <circle className="topology-gateway" cx="360" cy={centerY} r="31" />
    <text className="topology-center-label" x="360" y={centerY + 4} textAnchor="middle">{device.name.slice(0, 10)}</text>
  </svg></div>;
}

async function listDevices<T>() {
  const jwt = await account.createJWT();
  const response = await fetch(`${endpoint}/devices`, {
    headers: {
      "content-type": "application/json",
      "x-appwrite-project": projectId,
      "x-appwrite-jwt": jwt.jwt,
    },
  });
  const payload = await response.json() as T & { message?: string };
  if (!response.ok) throw new Error(payload.message || "Appwrite Devices request failed.");
  return payload;
}

async function deleteDevice(deviceId: string) {
  const jwt = await account.createJWT();
  const response = await fetch(`${endpoint}/devices/${encodeURIComponent(deviceId)}`, {
    method: "DELETE",
    headers: {
      "content-type": "application/json",
      "x-appwrite-project": projectId,
      "x-appwrite-jwt": jwt.jwt,
    },
  });
  if (!response.ok) {
    const payload = await response.json().catch(() => ({})) as { message?: string };
    throw new Error(payload.message || "Could not delete the Appwrite Device.");
  }
}

async function sendOtaCommand(deviceId: string, gatewayDeviceId?: string) {
  if (!otaImageUrl) throw new Error("This deployment does not expose a supported source repository for OTA.");
  const jwt = await account.createJWT();
  const response = await fetch(`${endpoint}/devices/${encodeURIComponent(deviceId)}/commands`, {
    method: "POST",
    headers: { "content-type": "application/json", "x-appwrite-project": projectId, "x-appwrite-jwt": jwt.jwt },
    body: JSON.stringify({ command: "ota", payload: { url: otaImageUrl, requestId: ID.unique() }, gatewayDeviceId }),
  });
  if (!response.ok) {
    const payload = await response.json().catch(() => ({})) as { message?: string };
    throw new Error(payload.message || "Could not send the OTA command.");
  }
}

async function sendRandomTemperatureScript(deviceId: string, requestId: string, gatewayDeviceId?: string) {
  const jwt = await account.createJWT();
  const response = await fetch(`${endpoint}/devices/${encodeURIComponent(deviceId)}/commands`, {
    method: "POST",
    headers: { "content-type": "application/json", "x-appwrite-project": projectId, "x-appwrite-jwt": jwt.jwt },
    body: JSON.stringify({
      command: "script",
      gatewayDeviceId,
      payload: {
        action: "download",
        requestId,
        intervalSeconds: randomTemperatureIntervalSeconds,
        script: randomTemperatureScript,
      },
    }),
  });
  if (!response.ok) {
    const payload = await response.json().catch(() => ({})) as { message?: string };
    throw new Error(payload.message || "Could not deploy the sensor script.");
  }
}

export default function Home() {
  const [user, setUser] = useState<Models.User<Models.Preferences> | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [telemetry, setTelemetry] = useState<Telemetry[]>([]);
  const [topology, setTopology] = useState<TopologyLink[]>([]);
  const [otaUpdates, setOtaUpdates] = useState<OtaUpdate[]>([]);
  const [selectedDeviceId, setSelectedDeviceId] = useState("");
  const [mobileDetailOpen, setMobileDetailOpen] = useState(false);
  const [historyRange, setHistoryRange] = useState<HistoryRange>("1h");
  const [historySeries, setHistorySeries] = useState<SensorSeries[]>([]);
  const [selectedSensorFields, setSelectedSensorFields] = useState<string[]>([]);
  const [historyReadings, setHistoryReadings] = useState<HistoryReading[]>([]);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState("");
  const [deleteConfirm, setDeleteConfirm] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [otaDeviceId, setOtaDeviceId] = useState("");
  const [otaMessage, setOtaMessage] = useState("");
  const [scriptDeviceId, setScriptDeviceId] = useState("");
  const [scriptMessage, setScriptMessage] = useState("");
  const [latestFirmwareVersion, setLatestFirmwareVersion] = useState("");
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState("");

  const refresh = useCallback(async () => {
    const [deviceResult, telemetryRows, topologyRows, otaRows] = await Promise.all([
      listDevices<{ devices: Device[] }>(),
      tables.listRows({ databaseId, tableId: telemetryTableId, queries: [Query.orderDesc("receivedAt"), Query.limit(500)] }),
      tables.listRows({ databaseId, tableId: topologyTableId, queries: [Query.equal("active", true), Query.greaterThanEqual("reportedAt", new Date(Date.now() - topologyRecentMs).toISOString()), Query.orderDesc("reportedAt"), Query.limit(500)] }),
      tables.listRows({ databaseId, tableId: otaUpdateTableId, queries: [Query.orderDesc("reportedAt"), Query.limit(500)] }),
    ]);
    setDevices(deviceResult.devices);
    setTelemetry(telemetryRows.rows as unknown as Telemetry[]);
    setTopology(topologyRows.rows as unknown as TopologyLink[]);
    setOtaUpdates(otaRows.rows as unknown as OtaUpdate[]);
  }, []);

  useEffect(() => {
    account.get().then(async (current) => { setUser(current); await refresh(); })
      .catch(() => undefined).finally(() => setBusy(false));
  }, [refresh]);

  useEffect(() => {
    if (!otaLatestReleaseApiUrl) return;
    let active = true;
    void fetch(otaLatestReleaseApiUrl, { headers: { Accept: "application/vnd.github+json" } })
      .then((response) => {
        if (!response.ok) throw new Error("Could not read the latest firmware release.");
        return response.json() as Promise<{ tag_name?: unknown }>;
      })
      .then((release) => {
        if (active && typeof release.tag_name === "string") setLatestFirmwareVersion(release.tag_name);
      })
      .catch(() => undefined);
    return () => { active = false; };
  }, []);

  useEffect(() => {
    if (!user) return;
    const timer = window.setInterval(() => void refresh().catch((caught) => setError(errorMessage(caught))), 5000);
    return () => window.clearInterval(timer);
  }, [refresh, user]);

  useEffect(() => {
    setSelectedDeviceId((current) => devices.some((device) => device.$id === current) ? current : devices[0]?.$id || "");
  }, [devices]);

  useEffect(() => {
    if (!selectedDeviceId) { setHistorySeries([]); setHistoryReadings([]); return; }
    let active = true;
    const duration = historyRanges.find((range) => range.key === historyRange)!.duration;
    setHistoryLoading(true); setHistoryError(""); setHistorySeries([]); setHistoryReadings([]);
    queryDeviceTimeseries(selectedDeviceId, duration).then((rows) => {
      if (!active) return;
      const series = timeseriesSensorSeries(rows, duration);
      setHistorySeries(series);
      setHistoryReadings(timeseriesReadings(rows));
      setSelectedSensorFields((current) => {
        const available = new Set(series.map((item) => item.field));
        const retained = current.filter((field) => available.has(field));
        return retained.length ? retained : series.slice(0, 3).map((item) => item.field);
      });
    }).catch((caught) => { if (active) setHistoryError(errorMessage(caught)); })
      .finally(() => { if (active) setHistoryLoading(false); });
    return () => { active = false; };
  }, [historyRange, selectedDeviceId]);

  async function authenticate(register: boolean) {
    setBusy(true); setError("");
    try {
      if (register) await account.create({ userId: ID.unique(), email, password });
      await account.createEmailPasswordSession({ email, password });
      setUser(await account.get());
      await refresh();
    } catch (caught) { setError(errorMessage(caught)); }
    finally { setBusy(false); }
  }

  async function signOut() {
    await account.deleteSession({ sessionId: "current" });
    setUser(null); setDevices([]); setTelemetry([]); setTopology([]); setOtaUpdates([]); setSelectedDeviceId(""); setMobileDetailOpen(false); setDeleteConfirm(false);
  }

  async function removeSelectedDevice() {
    if (!selectedDevice) return;
    setDeleting(true); setError("");
    try {
      await deleteDevice(selectedDevice.$id);
      setSelectedDeviceId(""); setMobileDetailOpen(false); setDeleteConfirm(false);
      await refresh();
    } catch (caught) { setError(errorMessage(caught)); }
    finally { setDeleting(false); }
  }

  async function updateSelectedDevice() {
    if (!selectedDevice) return;
    if (selectedFirmwareCurrent) {
      setOtaMessage(`Already running the latest firmware (${latestFirmwareVersion}).`);
      return;
    }
    if (selectedOtaPending) {
      setOtaMessage("An OTA update is already pending for this device.");
      return;
    }
    if (!window.confirm(`Install the latest HT-HC33 firmware on ${selectedDevice.name}?`)) return;
    setOtaDeviceId(selectedDevice.$id); setOtaMessage(""); setError("");
    try {
      await sendOtaCommand(selectedDevice.$id, mqttGatewayFor(selectedDevice, devices, topology));
      setOtaMessage("Update requested. The device will download, install, and restart in the background.");
    } catch (caught) { setError(errorMessage(caught)); }
    finally { setOtaDeviceId(""); }
  }

  async function deployRandomTemperatureSensor() {
    if (!selectedDevice) return;
    if (!window.confirm(`Replace the active sensor script on ${selectedDevice.name} with the random temperature sample?`)) return;
    setScriptDeviceId(selectedDevice.$id); setScriptMessage(""); setError("");
    try {
      await sendRandomTemperatureScript(selectedDevice.$id, ID.unique(), mqttGatewayFor(selectedDevice, devices, topology));
      setScriptMessage(`Command sent. The first random temperature reading should arrive now, then every ${randomTemperatureIntervalSeconds} seconds.`);
    } catch (caught) { setError(errorMessage(caught)); }
    finally { setScriptDeviceId(""); }
  }

  const latestVoltageByDevice = useMemo(() => {
    const latest = new Map<string, { row: Telemetry; value: number }>();
    for (const row of telemetry) {
      if (latest.has(row.deviceId)) continue;
      const value = voltageOf(row);
      if (value !== null) latest.set(row.deviceId, { row, value });
    }
    return latest;
  }, [telemetry]);
  const latestTelemetryByDevice = useMemo(() => {
    const latest = new Map<string, Telemetry>();
    for (const row of telemetry) if (!latest.has(row.deviceId)) latest.set(row.deviceId, row);
    return latest;
  }, [telemetry]);
  const latestTemperatureByDevice = useMemo(() => {
    const latest = new Map<string, { row: Telemetry; value: number }>();
    for (const row of telemetry) {
      if (latest.has(row.deviceId)) continue;
      const value = temperatureOf(row);
      if (value !== null) latest.set(row.deviceId, { row, value });
    }
    return latest;
  }, [telemetry]);
  const selectedDevice = devices.find((device) => device.$id === selectedDeviceId);
  const selectedLatestTelemetry = selectedDevice ? latestTelemetryByDevice.get(selectedDevice.$id) : undefined;
  const selectedGateway = selectedLatestTelemetry?.gatewayDeviceId
    ? devices.find((device) => device.$id === selectedLatestTelemetry.gatewayDeviceId) : undefined;
  const selectedLatest = selectedDevice ? latestVoltageByDevice.get(selectedDevice.$id) : undefined;
  const selectedTemperature = selectedDevice ? latestTemperatureByDevice.get(selectedDevice.$id) : undefined;
  const selectedStatus = selectedDevice ? statusOf(selectedDevice, selectedLatestTelemetry) : "";
  const selectedFirmwareVersion = firmwareVersionOf(selectedLatestTelemetry);
  const selectedTopology = topology.filter((link) => isRecentTopology(link) && (link.gatewayDeviceId === selectedDeviceId || link.peerDeviceId === selectedDeviceId));
  const selectedOtaUpdate = otaUpdates.find((update) => update.deviceId === selectedDeviceId);
  const selectedOtaPending = otaUpdates.some((update) => update.deviceId === selectedDeviceId && update.status === "pending");
  const selectedFirmwareCurrent = sameFirmwareVersion(selectedFirmwareVersion, latestFirmwareVersion);
  const selectedTopologyUpdatedAt = selectedTopology.reduce((latest, link) => link.reportedAt > latest ? link.reportedAt : latest, "");
  const activeRange = historyRanges.find((range) => range.key === historyRange)!;
  const selectedHistorySeries = historySeries.filter((series) => selectedSensorFields.includes(series.field));

  function toggleSensor(field: string) {
    setSelectedSensorFields((current) => current.includes(field)
      ? current.filter((candidate) => candidate !== field)
      : [...current, field]);
  }

  function selectDevice(device: Device) {
    setSelectedDeviceId(device.$id);
    setHistoryRange("1h");
    setDeleteConfirm(false);
    setScriptMessage("");
    setMobileDetailOpen(true);
  }

  return <main className={`shell ${mobileDetailOpen ? "mobile-showing-detail" : ""}`}>
    <header className="topbar"><span className="mark">L</span><strong>Live Stocking</strong>{user && <button className="link" onClick={signOut}>Sign out</button>}</header>
    {!user ? <section className="auth-grid">
      <div><p className="eyebrow">NEXT.JS · APPWRITE AUTH · MQTT</p><h1>Devices in.<br /><em>Signals out.</em></h1><p className="lede">Sign in to read the latest device state from TablesDB and permitted history from Time Series.</p></div>
      <form className="panel" onSubmit={(event) => { event.preventDefault(); void authenticate(false); }}>
        <h2>Operator access</h2>
        <label>Email<input type="email" value={email} onChange={(event) => setEmail(event.target.value)} required /></label>
        <label>Password<input type="password" value={password} onChange={(event) => setPassword(event.target.value)} minLength={8} required /></label>
        <div className="actions"><button disabled={busy}>Sign in</button><button className="secondary" type="button" disabled={busy} onClick={() => authenticate(true)}>Create account</button></div>
      </form>
    </section> : <section className="dashboard">
      <div className="heading-row"><div><p className="eyebrow">SIGNED IN AS {user.email}</p><h1>Device telemetry</h1></div><span className="count">{devices.length} DEVICES · TABLESDB LATEST · INFLUXDB HISTORY</span></div>
      <div className={`master-detail ${mobileDetailOpen ? "mobile-detail-open" : ""}`}>
        <aside className="device-master">
          <div className="master-heading"><div><p className="eyebrow">DEVICES</p><h2>Provisioned devices</h2></div><span>{devices.length}</span></div>
          <p className="master-help">Onboard new devices from the mobile app over BLE.</p>
          <div className="device-list">{devices.map((device) => {
            const latest = latestVoltageByDevice.get(device.$id);
            const status = statusOf(device, latestTelemetryByDevice.get(device.$id));
            return <button key={device.$id} className={`device-card ${selectedDeviceId === device.$id ? "selected" : ""}`} onClick={() => selectDevice(device)}>
              <span className="device-card-top"><span><strong>{device.name}</strong><code>{device.serial}</code></span><span className={`status ${status === "Online" ? "online" : "offline"}`}><i />{status}</span></span>
              <span className="device-value"><small>BATTERY VOLTAGE</small><b>{latest ? `${latest.value.toFixed(2)} V` : "—"}</b></span>
              <span className="device-seen">{latest ? `Updated ${relativeTime(latest.row.receivedAt)}` : "Waiting for telemetry"}<i>›</i></span>
            </button>;
          })}</div>
          {!devices.length && <p className="empty">No devices provisioned yet.</p>}
        </aside>
        <section className="device-detail">
          {selectedDevice ? <>
            <button className="mobile-back" onClick={() => { setDeleteConfirm(false); setMobileDetailOpen(false); }}>‹ All devices</button>
            <div className="detail-heading"><div><p className="eyebrow">DEVICE · {selectedDevice.serial}</p><h2>{selectedDevice.name}</h2></div><div className="detail-actions"><span className={`status ${selectedStatus === "Online" ? "online" : "offline"}`}><i />{selectedStatus}</span><button className="delete-device" onClick={() => setDeleteConfirm(true)}>Delete</button></div></div>
            <div className="telemetry-route"><span>TELEMETRY ROUTE</span><strong>{!selectedLatestTelemetry ? "Waiting for telemetry" : selectedLatestTelemetry.gatewayDeviceId ? `Via ${selectedGateway?.name || selectedGateway?.serial || selectedLatestTelemetry.gatewayDeviceId}` : "Direct MQTT"}</strong>{selectedGateway && <small>{selectedGateway.serial}</small>}</div>
            {deleteConfirm && <div className="delete-confirm" role="alert"><div><strong>Delete {selectedDevice.name}?</strong><p>This permanently removes the device and its MQTT credentials. Existing time-series history is not deleted.</p></div><div><button className="cancel-delete" onClick={() => setDeleteConfirm(false)} disabled={deleting}>Cancel</button><button className="confirm-delete" onClick={() => void removeSelectedDevice()} disabled={deleting}>{deleting ? "Deleting…" : "Delete device"}</button></div></div>}
            <div className="metric-card"><span>BATTERY VOLTAGE</span><strong>{selectedLatest ? `${selectedLatest.value.toFixed(2)} V` : "—"}</strong><small>{selectedLatest ? `Updated ${relativeTime(selectedLatest.row.receivedAt)}` : "No readings received"}</small></div>
            <div className="range-row"><span>HISTORY RANGE</span><div>{historyRanges.map((range) => <button key={range.key} className={historyRange === range.key ? "active" : ""} onClick={() => setHistoryRange(range.key)} disabled={historyLoading}>{range.label}</button>)}</div></div>
            <div className="chart-card"><header><div><h3>Sensor history</h3><p>InfluxDB · {selectedHistorySeries.reduce((total, series) => total + series.points.length, 0)} selected readings</p></div>{historyLoading && <span className="spinner" />}</header>
              <div className="sensor-picker" aria-label="Sensors displayed in the history chart">{historySeries.map((series) => <label key={series.field} className={selectedSensorFields.includes(series.field) ? "selected" : ""}><input type="checkbox" checked={selectedSensorFields.includes(series.field)} onChange={() => toggleSensor(series.field)} /><i style={{ background: series.color }} />{series.label}<small>{series.points.length}</small></label>)}{!historyLoading && !historySeries.length && <span>No sensor fields found.</span>}</div>
              <SensorChart series={selectedHistorySeries} duration={activeRange.duration} />
              <footer><span>{activeRange.label} ago</span><span>Now</span></footer>{historyError && <p className="inline-error">{historyError}</p>}
            </div>
            {selectedHistorySeries.length > 0 && <div className="series-stats">{selectedHistorySeries.map((series) => {
              const values = series.points.map((point) => point.value);
              const latest = series.points[series.points.length - 1];
              return <div key={series.field}><span><i style={{ background: series.color }} />{series.label}</span><strong>{latest.value.toFixed(2)}</strong><small>min {Math.min(...values).toFixed(2)} · avg {(values.reduce((sum, value) => sum + value, 0) / values.length).toFixed(2)} · max {Math.max(...values).toFixed(2)}</small></div>;
            })}</div>}
            <div className="metric-card temperature-metric"><span>TEMPERATURE · SENSOR TYPE 1</span><strong>{selectedTemperature ? `${selectedTemperature.value.toFixed(1)} °C` : "—"}</strong><small>{selectedTemperature ? `Updated ${relativeTime(selectedTemperature.row.receivedAt)}` : "No temperature readings received"}</small></div>
            <div className="topology-card"><header><div><h3>Mesh topology</h3><p>Direct HaLow links from reports received within the last 2 minutes</p></div><span>{selectedTopology.length} LINKS{selectedTopologyUpdatedAt ? ` · ${relativeTime(selectedTopologyUpdatedAt)}` : ""}</span></header><TopologyGraph device={selectedDevice} links={selectedTopology} /></div>
            {(selectedDevice.metadata?.firmwareTarget === "heltec-hc33" || selectedFirmwareVersion) && <div className="sensor-script-card"><div><h3>Random temperature sensor</h3><p>Installs a Lua sample derived from the EdgeZ sensor definition. It publishes sensor type 1 as a random 18.0–32.0 °C reading every {randomTemperatureIntervalSeconds} seconds and replaces the currently installed unified sensor script.</p>{selectedTemperature && <small>Latest temperature: {selectedTemperature.value.toFixed(1)} °C · {relativeTime(selectedTemperature.row.receivedAt)}</small>}{scriptMessage && <small>{scriptMessage}</small>}</div><button onClick={() => void deployRandomTemperatureSensor()} disabled={selectedStatus !== "Online" || scriptDeviceId === selectedDevice.$id}>{scriptDeviceId === selectedDevice.$id ? "Deploying…" : "Deploy sensor script"}</button></div>}
            {(selectedDevice.metadata?.firmwareTarget === "heltec-hc33" || selectedFirmwareVersion) && <div className="ota-card"><div><h3>Firmware update</h3><p>Running {selectedFirmwareVersion || "version unknown"}{latestFirmwareVersion ? `; latest ${latestFirmwareVersion}` : ""}. Install <code>live-stocking-ota.bin</code> from the latest release of this deployment&apos;s source repository.</p>{selectedOtaUpdate && <small>Latest update: {selectedOtaUpdate.status.toUpperCase()}{selectedOtaUpdate.detail ? ` · ${selectedOtaUpdate.detail}` : ""}{selectedOtaUpdate.reportedAt ? ` · ${relativeTime(selectedOtaUpdate.reportedAt)}` : ""}</small>}{otaMessage && <small>{otaMessage}</small>}</div><button onClick={() => void updateSelectedDevice()} disabled={!otaImageUrl || selectedStatus !== "Online" || selectedOtaPending || selectedFirmwareCurrent || otaDeviceId === selectedDevice.$id}>{otaDeviceId === selectedDevice.$id ? "Sending…" : selectedOtaPending ? "Update pending" : selectedFirmwareCurrent ? "Up to date" : "Update HT-HC33"}</button></div>}
            <div className="recent"><h3>Recent history</h3>{historyReadings.map((reading) => <article key={`${reading.timestamp}-${reading.channel}`}><header><code>{reading.channel}</code><time>{new Date(reading.timestamp).toLocaleString()}</time></header><pre>{prettyPayload(reading.payload)}</pre></article>)}{!historyReadings.length && <p className="empty">No time-series readings in this range.</p>}</div>
          </> : <p className="empty detail-empty">Select a device to see its telemetry.</p>}
        </section>
      </div>
    </section>}
    {error && <p className="error" role="alert">{error}</p>}
  </main>;
}
