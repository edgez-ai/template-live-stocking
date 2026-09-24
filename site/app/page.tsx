"use client";

import { Account, Client, ID, Models, Query, TablesDB } from "appwrite";
import { useCallback, useEffect, useMemo, useState } from "react";

type Device = { $id: string; serial: string; name: string; status: string; enabled: boolean };
type Telemetry = Models.Row & { deviceId: string; serial: string; channel: string; topic: string; payload: string; receivedAt: string };
type TopologyLink = Models.Row & { farmId: string; gatewayDeviceId: string; gatewaySerial: string; peerDeviceId: string; peerSerial: string; peerRadioMac: string; rssi?: number | null; active: boolean; lastSeenAt: string; reportedAt: string };
type HistoryRange = "30m" | "1h" | "6h" | "24h";
type VoltagePoint = { timestamp: number; value: number };
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
const endpoint = process.env.NEXT_PUBLIC_APPWRITE_ENDPOINT!.replace(/\/+$/, "");
const projectId = process.env.NEXT_PUBLIC_APPWRITE_PROJECT_ID!;
const historyRanges: { key: HistoryRange; label: string; duration: number }[] = [
  { key: "30m", label: "30 min", duration: 30 * 60 * 1000 },
  { key: "1h", label: "1 hour", duration: 60 * 60 * 1000 },
  { key: "6h", label: "6 hours", duration: 6 * 60 * 60 * 1000 },
  { key: "24h", label: "24 hours", duration: 24 * 60 * 60 * 1000 },
];
const topologyRecentMs = 2 * 60 * 1000;

function errorMessage(error: unknown) {
  return error instanceof Error ? error.message : "Something went wrong.";
}

const batteryVoltageSensorType = 12;

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

function prettyPayload(payload: string) {
  try { return JSON.stringify(JSON.parse(payload), null, 2); }
  catch { return payload; }
}

function VoltageChart({ points, duration }: { points: VoltagePoint[]; duration: number }) {
  const width = 720;
  const height = 260;
  const inset = 28;
  const sampled = points.length <= 240 ? points : points.filter((_, index) => index % Math.ceil(points.length / 240) === 0 || index === points.length - 1);
  if (!points.length) return <div className="chart-empty">No battery voltage data in this range.</div>;
  const values = points.map((point) => point.value);
  const rawMin = Math.min(...values);
  const rawMax = Math.max(...values);
  const padding = Math.max(0.05, (rawMax - rawMin) * .15);
  const min = rawMin - padding;
  const max = rawMax + padding;
  const end = Date.now();
  const start = end - duration;
  const coordinates = sampled.map((point) => ({
    x: inset + Math.max(0, Math.min(1, (point.timestamp - start) / duration)) * (width - inset * 2),
    y: inset + (1 - (point.value - min) / (max - min)) * (height - inset * 2),
  }));
  const path = coordinates.map((point) => `${point.x},${point.y}`).join(" ");
  const last = coordinates[coordinates.length - 1];

  return <div className="chart-wrap">
    <svg className="chart" viewBox={`0 0 ${width} ${height}`} role="img" aria-label="Battery voltage history line chart">
      {[0, 1, 2, 3].map((line) => <line key={line} className="chart-grid" x1={inset} x2={width - inset} y1={inset + line * (height - inset * 2) / 3} y2={inset + line * (height - inset * 2) / 3} />)}
      <polyline className="chart-line" points={path} />
      <circle className="chart-dot" cx={last.x} cy={last.y} r="5" />
      <text className="chart-label" x={width - 5} y={15} textAnchor="end">{rawMax.toFixed(2)} V</text>
      <text className="chart-label" x={width - 5} y={height - 5} textAnchor="end">{rawMin.toFixed(2)} V</text>
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

export default function Home() {
  const [user, setUser] = useState<Models.User<Models.Preferences> | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [telemetry, setTelemetry] = useState<Telemetry[]>([]);
  const [topology, setTopology] = useState<TopologyLink[]>([]);
  const [selectedDeviceId, setSelectedDeviceId] = useState("");
  const [mobileDetailOpen, setMobileDetailOpen] = useState(false);
  const [historyRange, setHistoryRange] = useState<HistoryRange>("1h");
  const [historyPoints, setHistoryPoints] = useState<VoltagePoint[]>([]);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState("");
  const [deleteConfirm, setDeleteConfirm] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState("");

  const refresh = useCallback(async () => {
    const [deviceResult, telemetryRows, topologyRows] = await Promise.all([
      listDevices<{ devices: Device[] }>(),
      tables.listRows({ databaseId, tableId: telemetryTableId, queries: [Query.orderDesc("receivedAt"), Query.limit(500)] }),
      tables.listRows({ databaseId, tableId: topologyTableId, queries: [Query.equal("active", true), Query.greaterThanEqual("reportedAt", new Date(Date.now() - topologyRecentMs).toISOString()), Query.orderDesc("reportedAt"), Query.limit(500)] }),
    ]);
    setDevices(deviceResult.devices);
    setTelemetry(telemetryRows.rows as unknown as Telemetry[]);
    setTopology(topologyRows.rows as unknown as TopologyLink[]);
  }, []);

  useEffect(() => {
    account.get().then(async (current) => { setUser(current); await refresh(); })
      .catch(() => undefined).finally(() => setBusy(false));
  }, [refresh]);

  useEffect(() => {
    if (!user) return;
    const timer = window.setInterval(() => void refresh().catch((caught) => setError(errorMessage(caught))), 5000);
    return () => window.clearInterval(timer);
  }, [refresh, user]);

  useEffect(() => {
    setSelectedDeviceId((current) => devices.some((device) => device.$id === current) ? current : devices[0]?.$id || "");
  }, [devices]);

  useEffect(() => {
    if (!selectedDeviceId) { setHistoryPoints([]); return; }
    let active = true;
    const duration = historyRanges.find((range) => range.key === historyRange)!.duration;
    setHistoryLoading(true); setHistoryError(""); setHistoryPoints([]);
    tables.listRows({
      databaseId,
      tableId: telemetryTableId,
      queries: [Query.equal("deviceId", selectedDeviceId), Query.greaterThanEqual("receivedAt", new Date(Date.now() - duration).toISOString()), Query.orderAsc("receivedAt"), Query.limit(5000)],
    }).then((result) => {
      if (!active) return;
      setHistoryPoints((result.rows as unknown as Telemetry[]).flatMap((row) => {
        const value = voltageOf(row);
        return value === null ? [] : [{ timestamp: new Date(row.receivedAt).getTime(), value }];
      }));
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
    setUser(null); setDevices([]); setTelemetry([]); setTopology([]); setSelectedDeviceId(""); setMobileDetailOpen(false); setDeleteConfirm(false);
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
  const selectedDevice = devices.find((device) => device.$id === selectedDeviceId);
  const selectedLatest = selectedDevice ? latestVoltageByDevice.get(selectedDevice.$id) : undefined;
  const selectedStatus = selectedDevice ? statusOf(selectedDevice, latestTelemetryByDevice.get(selectedDevice.$id)) : "";
  const selectedTelemetry = telemetry.filter((row) => row.deviceId === selectedDeviceId).slice(0, 10);
  const selectedTopology = topology.filter((link) => isRecentTopology(link) && (link.gatewayDeviceId === selectedDeviceId || link.peerDeviceId === selectedDeviceId));
  const selectedTopologyUpdatedAt = selectedTopology.reduce((latest, link) => link.reportedAt > latest ? link.reportedAt : latest, "");
  const activeRange = historyRanges.find((range) => range.key === historyRange)!;
  const historyStats = useMemo(() => {
    if (!historyPoints.length) return null;
    const values = historyPoints.map((point) => point.value);
    return { min: Math.min(...values), max: Math.max(...values), average: values.reduce((sum, value) => sum + value, 0) / values.length };
  }, [historyPoints]);

  function selectDevice(device: Device) {
    setSelectedDeviceId(device.$id);
    setHistoryRange("1h");
    setDeleteConfirm(false);
    setMobileDetailOpen(true);
  }

  return <main className={`shell ${mobileDetailOpen ? "mobile-showing-detail" : ""}`}>
    <header className="topbar"><span className="mark">L</span><strong>Live Stocking</strong>{user && <button className="link" onClick={signOut}>Sign out</button>}</header>
    {!user ? <section className="auth-grid">
      <div><p className="eyebrow">NEXT.JS · APPWRITE AUTH · MQTT</p><h1>Devices in.<br /><em>Signals out.</em></h1><p className="lede">Sign in to read your permitted device and telemetry rows directly from Appwrite.</p></div>
      <form className="panel" onSubmit={(event) => { event.preventDefault(); void authenticate(false); }}>
        <h2>Operator access</h2>
        <label>Email<input type="email" value={email} onChange={(event) => setEmail(event.target.value)} required /></label>
        <label>Password<input type="password" value={password} onChange={(event) => setPassword(event.target.value)} minLength={8} required /></label>
        <div className="actions"><button disabled={busy}>Sign in</button><button className="secondary" type="button" disabled={busy} onClick={() => authenticate(true)}>Create account</button></div>
      </form>
    </section> : <section className="dashboard">
      <div className="heading-row"><div><p className="eyebrow">SIGNED IN AS {user.email}</p><h1>Device telemetry</h1></div><span className="count">{devices.length} DEVICES · DIRECT TABLESDB READS</span></div>
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
            {deleteConfirm && <div className="delete-confirm" role="alert"><div><strong>Delete {selectedDevice.name}?</strong><p>This permanently removes the device and its MQTT credentials. Existing telemetry rows are not deleted.</p></div><div><button className="cancel-delete" onClick={() => setDeleteConfirm(false)} disabled={deleting}>Cancel</button><button className="confirm-delete" onClick={() => void removeSelectedDevice()} disabled={deleting}>{deleting ? "Deleting…" : "Delete device"}</button></div></div>}
            <div className="metric-card"><span>BATTERY VOLTAGE</span><strong>{selectedLatest ? `${selectedLatest.value.toFixed(2)} V` : "—"}</strong><small>{selectedLatest ? `Updated ${relativeTime(selectedLatest.row.receivedAt)}` : "No readings received"}</small></div>
            <div className="range-row"><span>HISTORY RANGE</span><div>{historyRanges.map((range) => <button key={range.key} className={historyRange === range.key ? "active" : ""} onClick={() => setHistoryRange(range.key)} disabled={historyLoading}>{range.label}</button>)}</div></div>
            <div className="chart-card"><header><div><h3>Battery voltage history</h3><p>Device battery ADC · {historyPoints.length} readings</p></div>{historyLoading && <span className="spinner" />}</header><VoltageChart points={historyPoints} duration={activeRange.duration} /><footer><span>{activeRange.label} ago</span><span>Now</span></footer>{historyError && <p className="inline-error">{historyError}</p>}</div>
            {historyStats && <div className="stats"><div><span>MIN</span><strong>{historyStats.min.toFixed(2)} V</strong></div><div><span>AVERAGE</span><strong>{historyStats.average.toFixed(2)} V</strong></div><div><span>MAX</span><strong>{historyStats.max.toFixed(2)} V</strong></div></div>}
            <p className="sensor-note">Battery voltage is measured by the device ADC; no reading appears when a battery is disconnected.</p>
            <div className="topology-card"><header><div><h3>Mesh topology</h3><p>Direct HaLow links from reports received within the last 2 minutes</p></div><span>{selectedTopology.length} LINKS{selectedTopologyUpdatedAt ? ` · ${relativeTime(selectedTopologyUpdatedAt)}` : ""}</span></header><TopologyGraph device={selectedDevice} links={selectedTopology} /></div>
            <div className="recent"><h3>Recent telemetry</h3>{selectedTelemetry.map((row) => <article key={row.$id}><header><code>{row.channel}</code><time>{new Date(row.receivedAt).toLocaleString()}</time></header><pre>{prettyPayload(row.payload)}</pre></article>)}{!selectedTelemetry.length && <p className="empty">No telemetry received yet.</p>}</div>
          </> : <p className="empty detail-empty">Select a device to see its telemetry.</p>}
        </section>
      </div>
    </section>}
    {error && <p className="error" role="alert">{error}</p>}
  </main>;
}
