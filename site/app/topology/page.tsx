"use client";

import { Account, Client, Models, Query, TablesDB } from "appwrite";
import dynamic from "next/dynamic";
import Link from "next/link";
import { useRouter } from "next/navigation";
import { useCallback, useEffect, useMemo, useState } from "react";
import type { CanvasLink, CanvasNode } from "./TopologyCanvas";

const TopologyCanvas = dynamic(() => import("./TopologyCanvas"), { ssr: false });

type Device = {
  $id: string;
  serial: string;
  name: string;
  enabled: boolean;
  metadata?: { farmId?: string; mqttGateway?: boolean; [key: string]: unknown };
};
type Telemetry = Models.Row & {
  deviceId: string;
  gatewayDeviceId?: string | null;
  halowMac?: string | null;
  receivedAt: string;
  payload: string;
};
type TopologyLink = Models.Row & {
  farmId: string;
  gatewayDeviceId: string;
  gatewaySerial: string;
  peerDeviceId: string;
  peerSerial: string;
  peerRadioMac: string;
  rssi?: number | null;
  active: boolean;
  lastSeenAt: string;
  reportedAt: string;
};
type GraphEntity = {
  id: string;
  name: string;
  serial: string;
  farmId: string;
  halowMac: string;
  kind: "gateway" | "device" | "unresolved";
  online: boolean;
  gatewayStatus: string;
  lastSeenAt: string;
  gatewayDeviceId: string;
};

const client = new Client()
  .setEndpoint(process.env.NEXT_PUBLIC_APPWRITE_ENDPOINT!)
  .setProject(process.env.NEXT_PUBLIC_APPWRITE_PROJECT_ID!);
const account = new Account(client);
const tables = new TablesDB(client);
const endpoint = process.env.NEXT_PUBLIC_APPWRITE_ENDPOINT!.replace(/\/+$/, "");
const projectId = process.env.NEXT_PUBLIC_APPWRITE_PROJECT_ID!;
const databaseId = process.env.NEXT_PUBLIC_DATABASE_ID!;
const telemetryTableId = process.env.NEXT_PUBLIC_TELEMETRY_TABLE_ID!;
const topologyTableId = process.env.NEXT_PUBLIC_TOPOLOGY_TABLE_ID!;
const topologyRecentMs = 2 * 60 * 1000;

function errorMessage(error: unknown) {
  return error instanceof Error ? error.message : "Something went wrong.";
}

function relativeTime(value: string) {
  if (!value) return "No telemetry";
  const seconds = Math.max(0, Math.round((Date.now() - new Date(value).getTime()) / 1000));
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  return `${Math.floor(seconds / 3600)}h ago`;
}

function payloadGatewayStatus(row?: Telemetry) {
  if (!row) return "unknown";
  try {
    const value = (JSON.parse(row.payload) as { gateway_status?: unknown }).gateway_status;
    return typeof value === "string" ? value : "unknown";
  } catch {
    return "unknown";
  }
}

function signalLabel(rssi?: number | null) {
  if (typeof rssi !== "number") return "RSSI unavailable";
  if (rssi >= -60) return "Excellent";
  if (rssi >= -75) return "Good";
  return "Weak";
}

async function listDevices() {
  const jwt = await account.createJWT();
  const response = await fetch(`${endpoint}/devices`, {
    headers: { "content-type": "application/json", "x-appwrite-project": projectId, "x-appwrite-jwt": jwt.jwt },
  });
  const payload = await response.json() as { devices?: Device[]; message?: string };
  if (!response.ok) throw new Error(payload.message || "Appwrite Devices request failed.");
  return payload.devices || [];
}

export default function TopologyPage() {
  const router = useRouter();
  const [user, setUser] = useState<Models.User<Models.Preferences> | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [telemetry, setTelemetry] = useState<Telemetry[]>([]);
  const [links, setLinks] = useState<TopologyLink[]>([]);
  const [farmId, setFarmId] = useState("all");
  const [selectedId, setSelectedId] = useState("");
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [updatedAt, setUpdatedAt] = useState("");
  const [error, setError] = useState("");

  const refresh = useCallback(async () => {
    setRefreshing(true);
    try {
      const [nextDevices, telemetryRows, topologyRows] = await Promise.all([
        listDevices(),
        tables.listRows({ databaseId, tableId: telemetryTableId, queries: [Query.orderDesc("receivedAt"), Query.limit(500)] }),
        tables.listRows({ databaseId, tableId: topologyTableId, queries: [Query.equal("active", true), Query.greaterThanEqual("reportedAt", new Date(Date.now() - topologyRecentMs).toISOString()), Query.orderDesc("reportedAt"), Query.limit(500)] }),
      ]);
      setDevices(nextDevices);
      setTelemetry(telemetryRows.rows as unknown as Telemetry[]);
      setLinks(topologyRows.rows as unknown as TopologyLink[]);
      setUpdatedAt(new Date().toISOString());
      setError("");
    } finally {
      setRefreshing(false);
    }
  }, []);

  useEffect(() => {
    let active = true;
    account.get().then(async (current) => {
      if (!active) return;
      setUser(current);
      try {
        await refresh();
      } catch (caught) {
        if (active) setError(errorMessage(caught));
      }
    }).catch(() => {
      if (active) router.replace("/");
    }).finally(() => {
      if (active) setLoading(false);
    });
    return () => { active = false; };
  }, [refresh, router]);

  useEffect(() => {
    if (!user) return;
    const timer = window.setInterval(() => void refresh().catch((caught) => setError(errorMessage(caught))), 5000);
    return () => window.clearInterval(timer);
  }, [refresh, user]);

  const latestByDevice = useMemo(() => {
    const latest = new Map<string, Telemetry>();
    for (const row of telemetry) if (!latest.has(row.deviceId)) latest.set(row.deviceId, row);
    return latest;
  }, [telemetry]);

  const farmIds = useMemo(() => [...new Set([
    ...devices.map((device) => device.metadata?.farmId || ""),
    ...links.map((link) => link.farmId),
  ].filter(Boolean))].sort(), [devices, links]);

  const graph = useMemo(() => {
    const farmLinks = links.filter((link) => farmId === "all" || link.farmId === farmId);
    const farmDevices = devices.filter((device) => farmId === "all" || device.metadata?.farmId === farmId);
    const gatewayIds = new Set(farmLinks.map((link) => link.gatewayDeviceId));
    for (const device of farmDevices) {
      const status = payloadGatewayStatus(latestByDevice.get(device.$id));
      if (device.metadata?.mqttGateway === true || status === "online" || status === "offline") gatewayIds.add(device.$id);
    }

    const entities = new Map<string, GraphEntity>();
    for (const device of farmDevices) {
      const latest = latestByDevice.get(device.$id);
      entities.set(device.$id, {
        id: device.$id,
        name: device.name,
        serial: device.serial,
        farmId: device.metadata?.farmId || "",
        halowMac: latest?.halowMac || "",
        kind: gatewayIds.has(device.$id) ? "gateway" : "device",
        online: Boolean(device.enabled && latest && Date.now() - new Date(latest.receivedAt).getTime() <= topologyRecentMs),
        gatewayStatus: payloadGatewayStatus(latest),
        lastSeenAt: latest?.receivedAt || "",
        gatewayDeviceId: latest?.gatewayDeviceId || "",
      });
    }
    for (const link of farmLinks) {
      if (!entities.has(link.gatewayDeviceId)) entities.set(link.gatewayDeviceId, {
        id: link.gatewayDeviceId, name: link.gatewaySerial, serial: link.gatewaySerial, farmId: link.farmId,
        halowMac: "", kind: "gateway", online: true, gatewayStatus: "unknown", lastSeenAt: link.reportedAt, gatewayDeviceId: "",
      });
      if (!entities.has(link.peerDeviceId)) entities.set(link.peerDeviceId, {
        id: link.peerDeviceId, name: link.peerSerial, serial: link.peerSerial, farmId: link.farmId,
        halowMac: link.peerRadioMac, kind: "unresolved", online: true, gatewayStatus: "unknown", lastSeenAt: link.lastSeenAt, gatewayDeviceId: link.gatewayDeviceId,
      });
    }

    const graphNodes: CanvasNode[] = [...entities.values()].map((entity) => ({
      id: entity.id,
      label: entity.name.length > 18 ? `${entity.name.slice(0, 17)}…` : entity.name,
      kind: entity.kind,
      online: entity.online,
    }));
    const graphLinks: CanvasLink[] = farmLinks.map((link) => ({
      id: link.$id,
      from: link.gatewayDeviceId,
      to: link.peerDeviceId,
      rssi: link.rssi,
    }));
    return { entities, nodes: graphNodes, links: graphLinks, rows: farmLinks };
  }, [devices, farmId, latestByDevice, links]);

  useEffect(() => {
    if (selectedId && !graph.entities.has(selectedId)) setSelectedId("");
  }, [graph.entities, selectedId]);

  const selected = graph.entities.get(selectedId);
  const selectedLinks = selected ? graph.rows.filter((link) => link.gatewayDeviceId === selected.id || link.peerDeviceId === selected.id) : [];
  const onlineCount = [...graph.entities.values()].filter((node) => node.online).length;
  const gatewayCount = [...graph.entities.values()].filter((node) => node.kind === "gateway").length;
  const rssiValues = graph.rows.flatMap((link) => typeof link.rssi === "number" ? [link.rssi] : []);
  const averageRssi = rssiValues.length ? Math.round(rssiValues.reduce((sum, value) => sum + value, 0) / rssiValues.length) : null;

  async function signOut() {
    await account.deleteSession({ sessionId: "current" });
    router.replace("/");
  }

  if (loading || !user) return <main className="shell topology-loading"><span className="spinner" /><p>Loading mesh topology…</p></main>;

  return <main className="shell topology-page">
    <header className="topbar"><span className="mark">L</span><strong>Live Stocking</strong><nav className="topnav" aria-label="Primary navigation"><Link href="/">Devices</Link><Link className="active" href="/topology">Topology</Link></nav><button className="link" onClick={() => void signOut()}>Sign out</button></header>
    <section className="topology-dashboard">
      <div className="topology-page-heading"><div><p className="eyebrow">LIVE HALOW MESH</p><h1>Network topology</h1><p>Drag nodes to explore the mesh. Scroll to zoom, drag the canvas to pan, and select a node for details.</p></div><div className="topology-controls"><label>Farm<select value={farmId} onChange={(event) => setFarmId(event.target.value)}><option value="all">All farms</option>{farmIds.map((id) => <option key={id} value={id}>{id}</option>)}</select></label><button onClick={() => void refresh().catch((caught) => setError(errorMessage(caught)))} disabled={refreshing}>{refreshing ? "Refreshing…" : "Refresh now"}</button><small>{updatedAt ? `Updated ${relativeTime(updatedAt)}` : "Waiting for data"}</small></div></div>
      <div className="topology-stats"><article><span>VISIBLE DEVICES</span><strong>{graph.entities.size}</strong></article><article><span>ONLINE NOW</span><strong>{onlineCount}</strong></article><article><span>GATEWAYS</span><strong>{gatewayCount}</strong></article><article><span>ACTIVE LINKS</span><strong>{graph.rows.length}</strong></article><article><span>AVERAGE SIGNAL</span><strong>{averageRssi === null ? "—" : `${averageRssi} dBm`}</strong></article></div>
      <div className="topology-workspace">
        <section className="topology-stage" aria-label="Interactive HaLow mesh topology">
          <div className="topology-legend"><span><i className="gateway" />Gateway</span><span><i className="online" />Online device</span><span><i className="offline" />Offline device</span><span><b className="signal excellent" />Excellent</span><span><b className="signal good" />Good</span><span><b className="signal weak" />Weak</span></div>
          {graph.nodes.length ? <TopologyCanvas nodes={graph.nodes} links={graph.links} selectedId={selectedId} onSelect={setSelectedId} /> : <div className="topology-zero"><strong>No topology data</strong><span>No devices or active HaLow links are visible for this farm.</span></div>}
        </section>
        <aside className="topology-inspector">
          {selected ? <><p className="eyebrow">SELECTED NODE</p><h2>{selected.name}</h2><code>{selected.serial}</code><div className="inspector-grid"><div><span>ROLE</span><strong>{selected.kind === "gateway" ? "MQTT gateway" : selected.kind === "unresolved" ? "Observed peer" : "Mesh device"}</strong></div><div><span>STATE</span><strong className={selected.online ? "available" : "unavailable"}>{selected.online ? "Online" : "Offline"}</strong></div><div><span>HALOW MAC</span><strong>{selected.halowMac || "Not reported"}</strong></div><div><span>LAST TELEMETRY</span><strong>{relativeTime(selected.lastSeenAt)}</strong></div><div><span>GATEWAY STATUS</span><strong>{selected.gatewayStatus}</strong></div><div><span>FARM</span><strong>{selected.farmId || "Unassigned"}</strong></div></div><h3>{selectedLinks.length} direct link{selectedLinks.length === 1 ? "" : "s"}</h3><div className="inspector-links">{selectedLinks.map((link) => { const peerSerial = link.gatewayDeviceId === selected.id ? link.peerSerial : link.gatewaySerial; return <article key={link.$id}><div><strong>{peerSerial}</strong><span>{relativeTime(link.lastSeenAt)}</span></div><b>{typeof link.rssi === "number" ? `${link.rssi} dBm` : "—"}<small>{signalLabel(link.rssi)}</small></b></article>; })}{!selectedLinks.length && <p>No active links in the last two minutes.</p>}</div></> : <div className="inspector-empty"><span>↖</span><strong>Select a node</strong><p>Device identity, gateway state, HaLow MAC, and direct RF links will appear here.</p></div>}
        </aside>
      </div>
      <section className="topology-table-card"><header><div><p className="eyebrow">RELATIONSHIPS</p><h2>Active HaLow links</h2></div><span>REPORTING WINDOW · 2 MINUTES</span></header><div className="topology-table-scroll"><table><thead><tr><th>Gateway</th><th>Peer</th><th>Peer HaLow MAC</th><th>RF signal</th><th>Quality</th><th>Last seen</th></tr></thead><tbody>{graph.rows.map((link) => <tr key={link.$id}><td>{link.gatewaySerial}</td><td>{link.peerSerial}</td><td><code>{link.peerRadioMac}</code></td><td>{typeof link.rssi === "number" ? `${link.rssi} dBm` : "—"}</td><td><span className={`signal-pill ${typeof link.rssi !== "number" ? "unknown" : link.rssi >= -60 ? "excellent" : link.rssi >= -75 ? "good" : "weak"}`}>{signalLabel(link.rssi)}</span></td><td>{relativeTime(link.lastSeenAt)}</td></tr>)}</tbody></table>{!graph.rows.length && <p className="empty">No active relationships reported.</p>}</div></section>
    </section>
    {error && <p className="error" role="alert">{error}</p>}
  </main>;
}
