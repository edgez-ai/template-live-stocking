"use client";

import { Account, Client, ID, Models, Query, TablesDB } from "appwrite";
import { useCallback, useEffect, useMemo, useState } from "react";

type Device = { $id: string; serial: string; name: string; status: string; enabled: boolean };
type Telemetry = Models.Row & { deviceId: string; serial: string; channel: string; topic: string; payload: string; receivedAt: string };
type HistoryRange = "30m" | "1h" | "6h" | "24h";
type TemperaturePoint = { timestamp: number; value: number };

const client = new Client()
  .setEndpoint(process.env.NEXT_PUBLIC_APPWRITE_ENDPOINT!)
  .setProject(process.env.NEXT_PUBLIC_APPWRITE_PROJECT_ID!);
const account = new Account(client);
const tables = new TablesDB(client);
const databaseId = process.env.NEXT_PUBLIC_DATABASE_ID!;
const telemetryTableId = process.env.NEXT_PUBLIC_TELEMETRY_TABLE_ID!;
const endpoint = process.env.NEXT_PUBLIC_APPWRITE_ENDPOINT!.replace(/\/+$/, "");
const projectId = process.env.NEXT_PUBLIC_APPWRITE_PROJECT_ID!;
const historyRanges: { key: HistoryRange; label: string; duration: number }[] = [
  { key: "30m", label: "30 min", duration: 30 * 60 * 1000 },
  { key: "1h", label: "1 hour", duration: 60 * 60 * 1000 },
  { key: "6h", label: "6 hours", duration: 6 * 60 * 60 * 1000 },
  { key: "24h", label: "24 hours", duration: 24 * 60 * 60 * 1000 },
];

function errorMessage(error: unknown) {
  return error instanceof Error ? error.message : "Something went wrong.";
}

function temperatureOf(row: Telemetry) {
  try {
    const value = Number((JSON.parse(row.payload) as { temperatureC?: unknown }).temperatureC);
    return Number.isFinite(value) ? value : null;
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

function prettyPayload(payload: string) {
  try { return JSON.stringify(JSON.parse(payload), null, 2); }
  catch { return payload; }
}

function TemperatureChart({ points, duration }: { points: TemperaturePoint[]; duration: number }) {
  const width = 720;
  const height = 260;
  const inset = 28;
  const sampled = points.length <= 240 ? points : points.filter((_, index) => index % Math.ceil(points.length / 240) === 0 || index === points.length - 1);
  if (!points.length) return <div className="chart-empty">No temperature data in this range.</div>;
  const values = points.map((point) => point.value);
  const rawMin = Math.min(...values);
  const rawMax = Math.max(...values);
  const padding = Math.max(1, (rawMax - rawMin) * .15);
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
    <svg className="chart" viewBox={`0 0 ${width} ${height}`} role="img" aria-label="Internal temperature history line chart">
      {[0, 1, 2, 3].map((line) => <line key={line} className="chart-grid" x1={inset} x2={width - inset} y1={inset + line * (height - inset * 2) / 3} y2={inset + line * (height - inset * 2) / 3} />)}
      <polyline className="chart-line" points={path} />
      <circle className="chart-dot" cx={last.x} cy={last.y} r="5" />
      <text className="chart-label" x={width - 5} y={15} textAnchor="end">{rawMax.toFixed(1)}°C</text>
      <text className="chart-label" x={width - 5} y={height - 5} textAnchor="end">{rawMin.toFixed(1)}°C</text>
    </svg>
  </div>;
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
  const [selectedDeviceId, setSelectedDeviceId] = useState("");
  const [mobileDetailOpen, setMobileDetailOpen] = useState(false);
  const [historyRange, setHistoryRange] = useState<HistoryRange>("1h");
  const [historyPoints, setHistoryPoints] = useState<TemperaturePoint[]>([]);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState("");
  const [deleteConfirm, setDeleteConfirm] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState("");

  const refresh = useCallback(async () => {
    const [deviceResult, telemetryRows] = await Promise.all([
      listDevices<{ devices: Device[] }>(),
      tables.listRows({ databaseId, tableId: telemetryTableId, queries: [Query.orderDesc("receivedAt"), Query.limit(500)] }),
    ]);
    setDevices(deviceResult.devices);
    setTelemetry(telemetryRows.rows as unknown as Telemetry[]);
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
        const value = temperatureOf(row);
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
    setUser(null); setDevices([]); setTelemetry([]); setSelectedDeviceId(""); setMobileDetailOpen(false); setDeleteConfirm(false);
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
  const selectedLatest = selectedDevice ? latestTemperatureByDevice.get(selectedDevice.$id) : undefined;
  const selectedStatus = selectedDevice ? statusOf(selectedDevice, selectedLatest?.row) : "";
  const selectedTelemetry = telemetry.filter((row) => row.deviceId === selectedDeviceId).slice(0, 10);
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
    <header className="topbar"><span className="mark">P</span><strong>IoT Provisioning</strong>{user && <button className="link" onClick={signOut}>Sign out</button>}</header>
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
            const latest = latestTemperatureByDevice.get(device.$id);
            const status = statusOf(device, latest?.row);
            return <button key={device.$id} className={`device-card ${selectedDeviceId === device.$id ? "selected" : ""}`} onClick={() => selectDevice(device)}>
              <span className="device-card-top"><span><strong>{device.name}</strong><code>{device.serial}</code></span><span className={`status ${status === "Online" ? "online" : "offline"}`}><i />{status}</span></span>
              <span className="device-value"><small>INTERNAL TEMPERATURE</small><b>{latest ? `${latest.value.toFixed(1)}°C` : "—"}</b></span>
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
            <div className="metric-card"><span>INTERNAL TEMPERATURE</span><strong>{selectedLatest ? `${selectedLatest.value.toFixed(1)}°C` : "—"}</strong><small>{selectedLatest ? `Updated ${relativeTime(selectedLatest.row.receivedAt)}` : "No readings received"}</small></div>
            <div className="range-row"><span>HISTORY RANGE</span><div>{historyRanges.map((range) => <button key={range.key} className={historyRange === range.key ? "active" : ""} onClick={() => setHistoryRange(range.key)} disabled={historyLoading}>{range.label}</button>)}</div></div>
            <div className="chart-card"><header><div><h3>Temperature history</h3><p>ESP32-S3 internal sensor · {historyPoints.length} readings</p></div>{historyLoading && <span className="spinner" />}</header><TemperatureChart points={historyPoints} duration={activeRange.duration} /><footer><span>{activeRange.label} ago</span><span>Now</span></footer>{historyError && <p className="inline-error">{historyError}</p>}</div>
            {historyStats && <div className="stats"><div><span>MIN</span><strong>{historyStats.min.toFixed(1)}°</strong></div><div><span>AVERAGE</span><strong>{historyStats.average.toFixed(1)}°</strong></div><div><span>MAX</span><strong>{historyStats.max.toFixed(1)}°</strong></div></div>}
            <p className="sensor-note">This is the ESP32-S3 chip temperature, not ambient room temperature.</p>
            <div className="recent"><h3>Recent telemetry</h3>{selectedTelemetry.map((row) => <article key={row.$id}><header><code>{row.channel}</code><time>{new Date(row.receivedAt).toLocaleString()}</time></header><pre>{prettyPayload(row.payload)}</pre></article>)}{!selectedTelemetry.length && <p className="empty">No telemetry received yet.</p>}</div>
          </> : <p className="empty detail-empty">Select a device to see its telemetry.</p>}
        </section>
      </div>
    </section>}
    {error && <p className="error" role="alert">{error}</p>}
  </main>;
}
