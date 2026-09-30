"use client";

import {
  mdiAccount, mdiBattery, mdiBellAlert, mdiBroadcast, mdiCamera, mdiCar, mdiCow,
  mdiCrosshairsGps, mdiDog, mdiDrone, mdiElectricSwitch, mdiHorse, mdiLanConnect,
  mdiMotionSensor, mdiPipeValve, mdiRouterWireless, mdiSheep, mdiSpeedometer,
  mdiTractor, mdiTruck, mdiWaterPump, mdiWifi,
} from "@mdi/js";
import ForceGraph2D, { ForceGraphMethods, GraphData, LinkObject, NodeObject } from "react-force-graph-2d";
import { useEffect, useMemo, useRef, useState } from "react";

export type CanvasNode = {
  id: string;
  label: string;
  kind: "gateway" | "device" | "unresolved";
  online: boolean;
  icon: string;
  color: string;
  wifiOnline: boolean;
  internetOnline: boolean;
  mqttDirect: boolean;
};

export type CanvasLink = {
  id: string;
  from: string;
  to: string;
  rssi?: number | null;
};

type GraphNode = CanvasNode;
type GraphLink = {
  id: string;
  rssi?: number | null;
};

function signalColor(rssi?: number | null) {
  if (typeof rssi !== "number") return "#91a7a3";
  if (rssi >= -60) return "#0a9b7d";
  if (rssi >= -75) return "#d68a24";
  return "#d05b47";
}

const iconPaths: Record<string, string> = {
  sheep: mdiSheep, cow: mdiCow, goat: mdiSheep, horse: mdiHorse, dog: mdiDog, person: mdiAccount,
  tractor: mdiTractor, truck: mdiTruck, car: mdiCar, drone: mdiDrone, router: mdiRouterWireless,
  gateway: mdiLanConnect, beacon: mdiBroadcast, tracker: mdiCrosshairsGps, sensor: mdiMotionSensor,
  camera: mdiCamera, gps: mdiCrosshairsGps, meter: mdiSpeedometer, pump: mdiWaterPump,
  valve: mdiPipeValve, switch: mdiElectricSwitch, battery: mdiBattery, alarm: mdiBellAlert,
  __wifi: mdiWifi,
};
const canvasPaths = new Map<string, Path2D>();

function pathForIcon(icon: string) {
  const path = iconPaths[icon] || mdiCrosshairsGps;
  let canvasPath = canvasPaths.get(path);
  if (!canvasPath) {
    canvasPath = new Path2D(path);
    canvasPaths.set(path, canvasPath);
  }
  return canvasPath;
}

export default function TopologyCanvas({
  nodes,
  links,
  selectedId,
  onSelect,
}: {
  nodes: CanvasNode[];
  links: CanvasLink[];
  selectedId: string;
  onSelect: (id: string) => void;
}) {
  const containerRef = useRef<HTMLDivElement>(null);
  const graphRef = useRef<ForceGraphMethods<GraphNode, GraphLink> | undefined>(undefined);
  const fittedGraphRef = useRef("");
  const [size, setSize] = useState({ width: 900, height: 650 });
  const graphKey = useMemo(() => `${nodes.map((node) => node.id).sort().join(",")}|${links.map((link) => link.id).sort().join(",")}`, [links, nodes]);

  useEffect(() => {
    const element = containerRef.current;
    if (!element) return;
    const update = () => setSize({ width: element.clientWidth || 900, height: element.clientHeight || 650 });
    update();
    const observer = new ResizeObserver(update);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  const graphData = useMemo<GraphData<GraphNode, GraphLink>>(() => ({
    nodes: nodes.map((node) => ({ ...node })),
    links: links.map((link) => ({ id: link.id, source: link.from, target: link.to, rssi: link.rssi })),
  }), [links, nodes]);

  function paintNode(node: NodeObject<GraphNode>, context: CanvasRenderingContext2D, scale: number) {
    const x = node.x || 0;
    const y = node.y || 0;
    const radius = node.kind === "gateway" ? 6.5 : node.kind === "unresolved" ? 4.5 : 5.5;
    if (node.id === selectedId) {
      context.beginPath();
      context.arc(x, y, radius + 2.3, 0, Math.PI * 2);
      context.fillStyle = "#ffcf5c";
      context.fill();
    }
    if (node.kind === "gateway") {
      context.beginPath();
      context.arc(x, y, radius + 1.25, 0, Math.PI * 2);
      context.lineWidth = 1.25 / scale;
      context.strokeStyle = "#173f42";
      context.stroke();
    }
    context.beginPath();
    context.arc(x, y, radius, 0, Math.PI * 2);
    context.fillStyle = node.color;
    context.fill();
    context.lineWidth = 1.2 / scale;
    context.strokeStyle = "#ffffff";
    context.stroke();

    const annotationRadius = Math.max(1.7, radius * .29);
    const annotations = [
      { x: node.mqttDirect ? x - radius * .45 : x, icon: "__wifi", active: node.wifiOnline, color: "#247cbd" },
      ...(node.mqttDirect ? [{ x: x + radius * .45, icon: "gateway", active: true, color: "#7849a8" }] : []),
    ];
    for (const annotation of annotations) {
      const annotationY = y + radius * .85;
      context.beginPath();
      context.arc(annotation.x, annotationY, annotationRadius, 0, Math.PI * 2);
      context.fillStyle = annotation.active ? annotation.color : "#8b9f9c";
      context.fill();
      context.lineWidth = .7 / scale;
      context.strokeStyle = "#ffffff";
      context.stroke();
      const badgeIconSize = annotationRadius * 1.35;
      context.save();
      context.translate(annotation.x - badgeIconSize / 2, annotationY - badgeIconSize / 2);
      context.scale(badgeIconSize / 24, badgeIconSize / 24);
      context.fillStyle = "#ffffff";
      context.fill(pathForIcon(annotation.icon));
      context.restore();
    }

    const iconSize = radius * 1.25;
    context.save();
    context.translate(x - iconSize / 2, y - iconSize / 2);
    context.scale(iconSize / 24, iconSize / 24);
    context.fillStyle = "#ffffff";
    context.fill(pathForIcon(node.icon));
    context.restore();

    context.beginPath();
    context.arc(x + radius * .72, y - radius * .72, Math.max(1.15, radius * .22), 0, Math.PI * 2);
    context.fillStyle = node.online ? "#16a085" : "#8b9f9c";
    context.fill();
    context.lineWidth = .8 / scale;
    context.strokeStyle = "#ffffff";
    context.stroke();

    const fontSize = 9 / scale;
    context.font = `800 ${fontSize}px Inter, ui-sans-serif, system-ui, sans-serif`;
    context.textAlign = "center";
    context.textBaseline = "middle";
    const labelY = y + radius + 9 / scale;
    const textWidth = context.measureText(node.label).width;
    context.fillStyle = "rgba(255, 255, 255, .9)";
    context.fillRect(x - textWidth / 2 - 3 / scale, labelY - fontSize / 2 - 2 / scale, textWidth + 6 / scale, fontSize + 4 / scale);
    context.fillStyle = "#173f42";
    context.fillText(node.label, x, labelY);
  }

  function paintLinkLabel(link: LinkObject<GraphNode, GraphLink>, context: CanvasRenderingContext2D, scale: number) {
    const source = typeof link.source === "object" ? link.source : undefined;
    const target = typeof link.target === "object" ? link.target : undefined;
    if (!source || !target || source.x === undefined || source.y === undefined || target.x === undefined || target.y === undefined) return;
    const label = typeof link.rssi === "number" ? `${link.rssi} dBm` : "HaLow";
    const x = (source.x + target.x) / 2;
    const y = (source.y + target.y) / 2;
    const fontSize = 9 / scale;
    context.font = `800 ${fontSize}px Inter, ui-sans-serif, system-ui, sans-serif`;
    context.textAlign = "center";
    context.textBaseline = "middle";
    const width = context.measureText(label).width;
    context.fillStyle = "rgba(245, 249, 248, .94)";
    context.fillRect(x - width / 2 - 3 / scale, y - fontSize / 2 - 2 / scale, width + 6 / scale, fontSize + 4 / scale);
    context.fillStyle = signalColor(link.rssi);
    context.fillText(label, x, y);
  }

  return <div className="topology-canvas" ref={containerRef}>
    <ForceGraph2D<GraphNode, GraphLink>
      ref={graphRef}
      width={size.width}
      height={size.height}
      graphData={graphData}
      backgroundColor="rgba(0,0,0,0)"
      nodeCanvasObject={paintNode}
      nodePointerAreaPaint={(node, color, context) => { context.beginPath(); context.arc(node.x || 0, node.y || 0, node.kind === "gateway" ? 9 : 7, 0, Math.PI * 2); context.fillStyle = color; context.fill(); }}
      nodeLabel={(node) => `${node.label} · ${node.kind === "gateway" ? "Gateway" : node.online ? "Online" : "Offline"} · Internet ${node.internetOnline ? "reachable" : "unavailable"} · Wi-Fi ${node.wifiOnline ? "connected" : "no active link"}${node.mqttDirect ? " · MQTT direct" : ""}`}
      linkColor={(link) => signalColor(link.rssi)}
      linkWidth={(link) => typeof link.rssi === "number" && link.rssi < -75 ? 1.5 : 2.5}
      linkDirectionalArrowLength={4}
      linkDirectionalArrowRelPos={.74}
      linkDirectionalArrowColor={(link) => signalColor(link.rssi)}
      linkCanvasObjectMode={() => "after"}
      linkCanvasObject={paintLinkLabel}
      linkLabel={(link) => typeof link.rssi === "number" ? `RF signal ${link.rssi} dBm` : "RF signal unavailable"}
      minZoom={.15}
      maxZoom={6}
      warmupTicks={80}
      cooldownTicks={180}
      d3VelocityDecay={.28}
      onEngineStop={() => {
        if (fittedGraphRef.current === graphKey) return;
        fittedGraphRef.current = graphKey;
        graphRef.current?.zoomToFit(450, 90);
      }}
      onNodeClick={(node) => onSelect(String(node.id || ""))}
      onBackgroundClick={() => onSelect("")}
      enableNodeDrag
      enablePanInteraction
      enableZoomInteraction
    />
  </div>;
}
