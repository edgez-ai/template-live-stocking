import Constants from "expo-constants";
import { StatusBar } from "expo-status-bar";
import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { ActivityIndicator, Alert, KeyboardAvoidingView, Modal, PermissionsAndroid, Platform, Pressable, ScrollView, StyleSheet, Text, TextInput, View } from "react-native";
import type { LayoutChangeEvent } from "react-native";
import { SafeAreaProvider, SafeAreaView } from "react-native-safe-area-context";
import { Account, Client, ID, Models, Permission, Query, Role, Roles, TablesDB, Teams } from "react-native-appwrite";
import { ESPDevice, ESPProvisionManager, ESPSecurity, ESPTransport } from "@orbital-systems/react-native-esp-idf-provisioning";
import type { ESPWifiList } from "@orbital-systems/react-native-esp-idf-provisioning";
import { EdgezOrganicMap, edgezMapIcons } from "@edgez/react-native-sdk";
import type { EdgezMapDownloadUpdate, EdgezMapIcon, EdgezMapLine, EdgezMapNode, EdgezOrganicMapRef } from "@edgez/react-native-sdk";
import Ionicons from "@expo/vector-icons/Ionicons";
import MaterialCommunityIcons from "@expo/vector-icons/MaterialCommunityIcons";
import AsyncStorage from "@react-native-async-storage/async-storage";
import * as Location from "expo-location";
import { centerChannelForCountry, channelsForCountry, halowCountries } from "./halowChannels";
import { NrfProvisioningDevice, scanNrfProvisioningDevices } from "./nrfProvisioning";

type ProvisioningDevice = ESPDevice | NrfProvisioningDevice;
function isNrfDevice(device: ProvisioningDevice): device is NrfProvisioningDevice {
  return device instanceof NrfProvisioningDevice;
}

type Device = { $id: string; serial: string; name: string; status: string; enabled: boolean; metadata?: { farmId?: string; icon?: EdgezMapIcon; markerColor?: MapMarkerColor; latitude?: number; longitude?: number; [key: string]: unknown }; latitude?: number; longitude?: number };
type Farm = Models.Row & { name: string; country: string; location: string; halowChannel: number; meshId: string; meshPassphrase: string; teamId: string; ownerId: string };
type CurrentUser = Pick<Models.User<Models.Preferences>, "$id" | "email" | "prefs"> & { name?: string };
type CachedFarm = Pick<Farm, "$id" | "name" | "country" | "location" | "halowChannel" | "meshId" | "teamId" | "ownerId">;
type CachedSnapshot = { version: 1; user: CurrentUser; farms: CachedFarm[]; devices: Device[]; telemetry: CachedTelemetry[] };
type FarmDetails = { name: string; country: string; location: string; halowChannel: string; meshId: string; meshPassphrase: string };
type Credential = { clientId: string; username: string; password: string };
type Telemetry = Models.Row & { deviceId: string; serial: string; channel: string; topic: string; payload: string; receivedAt: string };
type TopologyLink = Models.Row & { farmId: string; gatewayDeviceId: string; gatewaySerial: string; peerDeviceId: string; peerSerial: string; peerRadioMac: string; rssi?: number | null; active: boolean; lastSeenAt: string; reportedAt: string };
type OtaUpdate = Models.Row & { deviceId: string; serial: string; requestId: string; status: "pending" | "succeeded" | "failed" | "busy"; detail?: string; firmwareVersion?: string; targetFirmwareVersion?: string; reportedAt: string; completedAt?: string | null };
type GeofenceShape = "circle" | "oval" | "rectangle" | "polygon";
type GeofenceArea = Models.Row & { farmId: string; name: string; shape: GeofenceShape; geometry: string };
type GeofenceRule = Models.Row & { farmId: string; name: string; areaId: string; deviceIds: string[]; enterAlert: boolean; exitAlert: boolean };
type GeofenceAlarm = Models.Row & { farmId: string; areaId: string; ruleId: string; deviceId: string; event: "enter" | "exit"; active: boolean; acknowledged: boolean; lastLocation: string; raisedAt: string; clearedAt?: string; acknowledgedAt?: string };
type CachedTelemetry = Pick<Telemetry, "$id" | "deviceId" | "serial" | "channel" | "topic" | "payload" | "receivedAt">;
type AppConfig = { appwriteEndpoint: string; appwriteProjectId: string; appwritePlatform: string; teamInviteUrl?: string; otaRepositoryUrl?: string; databaseId: string; telemetryTableId: string; topologyTableId: string; otaUpdateTableId: string; farmTableId: string; geofenceAreaTableId: string; geofenceRuleTableId: string; geofenceAlarmTableId: string };
type HistoryRange = "30m" | "1h" | "6h" | "24h";
type DashboardView = "map" | "list";
type DeviceLocationChoice = "none" | "current" | "map" | "gps";
type VoltagePoint = { timestamp: number; value: number };
type AreaDraft = { name: string; shape: GeofenceShape; location: string; primary: string; secondary: string; vertices: string; color: string };
type GeofenceGeometry = { center?: { latitude: number; longitude: number }; radiusMeters?: number; radiusXMeters?: number; radiusYMeters?: number; widthMeters?: number; heightMeters?: number; rotationDegrees?: number; vertices?: { latitude: number; longitude: number }[]; color?: string };
type SettingsTab = "team" | "areas" | "rules";
const emptyFarmDetails: FarmDetails = { name: "", country: "", location: "", halowChannel: "", meshId: "", meshPassphrase: "" };
const defaultAreaColor = "#E88D29";
const emptyAreaDraft: AreaDraft = { name: "", shape: "circle", location: "", primary: "100", secondary: "100", vertices: "", color: defaultAreaColor };
const iconGlyphs: Record<EdgezMapIcon, React.ComponentProps<typeof MaterialCommunityIcons>["name"]> = {
  sheep: "sheep", cow: "cow", goat: "sheep", horse: "horse", dog: "dog", person: "account",
  tractor: "tractor", truck: "truck", car: "car", drone: "drone", router: "router-wireless",
  gateway: "lan-connect", beacon: "broadcast", tracker: "crosshairs-gps", sensor: "motion-sensor",
  camera: "camera", gps: "crosshairs-gps", meter: "speedometer", pump: "water-pump",
  valve: "pipe-valve", switch: "electric-switch", battery: "battery", alarm: "bell-alert",
};
const mapMarkerColors = [
  { key: "red", label: "Red", hex: "#E51B23" }, { key: "pink", label: "Pink", hex: "#FF4182" },
  { key: "purple", label: "Purple", hex: "#9B24B2" }, { key: "deep_purple", label: "Deep purple", hex: "#6639BF" },
  { key: "blue", label: "Blue", hex: "#0066CC" }, { key: "light_blue", label: "Light blue", hex: "#249CF2" },
  { key: "cyan", label: "Cyan", hex: "#14BECD" }, { key: "teal", label: "Teal", hex: "#00A58C" },
  { key: "green", label: "Green", hex: "#3C8C3C" }, { key: "lime", label: "Lime", hex: "#93BF39" },
  { key: "yellow", label: "Yellow", hex: "#FFC800" }, { key: "orange", label: "Orange", hex: "#FF9600" },
  { key: "deep_orange", label: "Deep orange", hex: "#F06432" }, { key: "brown", label: "Brown", hex: "#804633" },
  { key: "gray", label: "Gray", hex: "#737373" }, { key: "blue_gray", label: "Blue gray", hex: "#597380" },
] as const;
type MapMarkerColor = (typeof mapMarkerColors)[number]["key"];
const areaColors = [{ key: "orange", label: "Orange", hex: defaultAreaColor }, ...mapMarkerColors.filter(({ key }) => key !== "orange")];

function areaColor(value: unknown): string {
  return areaColors.find(({ hex }) => hex === value)?.hex || defaultAreaColor;
}

function savedAreaColor(area: GeofenceArea): string {
  try { return areaColor(JSON.parse(area.geometry).color); }
  catch { return defaultAreaColor; }
}

function colorForDevice(device: Device): MapMarkerColor {
  const saved = device.metadata?.markerColor;
  return mapMarkerColors.some(({ key }) => key === saved) ? saved as MapMarkerColor : device.enabled ? "blue" : "gray";
}

function MapAppearancePicker({ icon, color, onIconChange, onColorChange }: { icon: EdgezMapIcon; color: MapMarkerColor; onIconChange: (icon: EdgezMapIcon) => void; onColorChange: (color: MapMarkerColor) => void }) {
  const selectedHex = mapMarkerColors.find(({ key }) => key === color)?.hex || "#0066CC";
  return <>
    <Text style={styles.fieldLabel}>MAP ICON</Text>
    <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.iconChoices}>
      {edgezMapIcons.map((choice) => <Pressable key={choice} style={[styles.iconChoice, icon === choice && styles.farmRowSelected]} onPress={() => onIconChange(choice)} accessibilityRole="radio" accessibilityState={{ selected: icon === choice }} accessibilityLabel={`${choice} map icon`}>
        <MaterialCommunityIcons name={iconGlyphs[choice]} size={24} color={icon === choice ? selectedHex : "#0a3037"} />
        <Text style={styles.deviceNameDark}>{choice.toUpperCase()}</Text>
      </Pressable>)}
    </ScrollView>
    <Text style={styles.fieldLabel}>MAP COLOR</Text>
    <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.iconChoices}>
      {mapMarkerColors.map((choice) => <Pressable key={choice.key} style={[styles.iconChoice, color === choice.key && styles.farmRowSelected]} onPress={() => onColorChange(choice.key)} accessibilityRole="radio" accessibilityState={{ selected: color === choice.key }} accessibilityLabel={`${choice.label} map color`}>
        <MaterialCommunityIcons name={iconGlyphs[icon]} size={24} color={choice.hex} />
        <Text style={styles.deviceNameDark}>{choice.label}</Text>
      </Pressable>)}
    </ScrollView>
  </>;
}

function AreaColorPicker({ color, onChange }: { color: string; onChange: (color: string) => void }) {
  return <>
    <Text style={styles.fieldLabel}>AREA COLOR</Text>
    <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.iconChoices}>
      {areaColors.map((choice) => <Pressable key={choice.key} style={[styles.iconChoice, color === choice.hex && styles.farmRowSelected]} onPress={() => onChange(choice.hex)} accessibilityRole="radio" accessibilityState={{ selected: color === choice.hex }} accessibilityLabel={`${choice.label} area color`}>
        <View style={[styles.areaColorSwatch, { backgroundColor: choice.hex }]} />
        <Text style={styles.deviceNameDark}>{choice.label}</Text>
      </Pressable>)}
    </ScrollView>
  </>;
}

function detailsFromFarm(farm?: Farm): FarmDetails {
  return farm ? { name: farm.name, country: farm.country, location: farm.location, halowChannel: String(farm.halowChannel), meshId: farm.meshId, meshPassphrase: farm.meshPassphrase } : emptyFarmDetails;
}

function farmData(details: FarmDetails) {
  const country = details.country.trim().toUpperCase();
  const channel = Number(details.halowChannel);
  if (!details.name.trim() || !coordinatesFromLocation(details.location) || !halowCountries.some((item) => item.code === country) ||
      !channelsForCountry(country).some((item) => item.number === channel) ||
      !details.meshId.trim() || details.meshId.trim().length > 32 ||
      details.meshPassphrase.length < 8 || details.meshPassphrase.length > 63) {
    throw new Error("Enter a name, choose a map location, country and 1 MHz channel, and provide a mesh ID and an 8–63 character passphrase.");
  }
  return { name: details.name.trim(), country, location: details.location.trim(), halowChannel: channel, meshId: details.meshId.trim(), meshPassphrase: details.meshPassphrase };
}

function coordinatesFromLocation(location: string) {
  const match = location.match(/^\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*$/);
  if (!match) return null;
  const latitude = Number(match[1]);
  const longitude = Number(match[2]);
  return latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180 ? { latitude, longitude } : null;
}

function areaGeometry(draft: AreaDraft) {
  const vertices = draft.vertices.split(";").map((value) => coordinatesFromLocation(value)).filter((value): value is { latitude: number; longitude: number } => Boolean(value));
  const center = draft.shape === "polygon" && vertices.length
    ? { latitude: vertices.reduce((sum, point) => sum + point.latitude, 0) / vertices.length, longitude: vertices.reduce((sum, point) => sum + point.longitude, 0) / vertices.length }
    : coordinatesFromLocation(draft.location);
  const primary = Number(draft.primary);
  const secondary = Number(draft.secondary);
  if (!draft.name.trim() || !center) throw new Error("Enter an area name and choose its position on the map.");
  if (draft.shape !== "polygon" && (!Number.isFinite(primary) || primary < 10 || primary > 100000)) throw new Error("Set a valid area size in meters.");
  const color = areaColor(draft.color);
  if (draft.shape === "circle") return JSON.stringify({ center, radiusMeters: primary, color });
  if (draft.shape === "oval") {
    if (!Number.isFinite(secondary) || secondary < 10 || secondary > 100000) throw new Error("Enter both oval radii in meters.");
    return JSON.stringify({ center, radiusXMeters: primary, radiusYMeters: secondary, rotationDegrees: 0, color });
  }
  if (draft.shape === "rectangle") {
    if (!Number.isFinite(secondary) || secondary < 10 || secondary > 100000) throw new Error("Enter rectangle width and height in meters.");
    return JSON.stringify({ center, widthMeters: primary, heightMeters: secondary, rotationDegrees: 0, color });
  }
  if (new Set(vertices.map((point) => `${point.latitude.toFixed(6)},${point.longitude.toFixed(6)}`)).size < 3) throw new Error("Add at least three distinct polygon points on the map.");
  return JSON.stringify({ center, vertices, color });
}

function geofenceBoundaryPoints(shape: GeofenceShape, geometry: GeofenceGeometry) {
  if (shape === "polygon") {
    return (geometry.vertices || []).filter((point) => Number.isFinite(point.latitude) && Number.isFinite(point.longitude) && Math.abs(point.latitude) <= 90 && Math.abs(point.longitude) <= 180);
  }
  const center = geometry.center;
  if (!center || !Number.isFinite(center.latitude) || !Number.isFinite(center.longitude)) return [];
  const radiusX = shape === "circle" ? Number(geometry.radiusMeters) : shape === "oval" ? Number(geometry.radiusXMeters) : Number(geometry.widthMeters) / 2;
  const radiusY = shape === "circle" ? radiusX : shape === "oval" ? Number(geometry.radiusYMeters) : Number(geometry.heightMeters) / 2;
  if (!Number.isFinite(radiusX) || !Number.isFinite(radiusY) || radiusX <= 0 || radiusY <= 0) return [];
  const rotation = (Number(geometry.rotationDegrees) || 0) * Math.PI / 180;
  const corners = shape === "rectangle" ? [[-radiusX, -radiusY], [radiusX, -radiusY], [radiusX, radiusY], [-radiusX, radiusY]] : Array.from({ length: 48 }, (_, index) => { const angle = index * Math.PI / 24; return [radiusX * Math.cos(angle), radiusY * Math.sin(angle)]; });
  return corners.map(([east, north]) => {
    const rotatedEast = east * Math.cos(rotation) - north * Math.sin(rotation);
    const rotatedNorth = east * Math.sin(rotation) + north * Math.cos(rotation);
    return { latitude: center.latitude + rotatedNorth / 111320, longitude: center.longitude + rotatedEast / (111320 * Math.max(0.01, Math.cos(center.latitude * Math.PI / 180))) };
  });
}

function geofenceLine(area: GeofenceArea): EdgezMapLine | null {
  try {
    const geometry = JSON.parse(area.geometry) as GeofenceGeometry;
    const points = geofenceBoundaryPoints(area.shape, geometry);
    if (points.length < 3) return null;
    return { id: area.$id, points: [...points, points[0]], color: areaColor(geometry.color) };
  } catch { return null; }
}

const sensorType = { latitude: 3, longitude: 4, batteryVoltage: 12 } as const;
type SensorPayload = { sensors?: { type?: unknown; value?: unknown }[] };

function sensorValue(payload: SensorPayload, type: number) {
  const value = payload.sensors?.find((sensor) => sensor?.type === type)?.value;
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function telemetryCoordinates(row?: Telemetry) {
  if (!row) return null;
  let payload: SensorPayload;
  try { payload = JSON.parse(row.payload); } catch { return null; }
  const latitude = sensorValue(payload, sensorType.latitude);
  const longitude = sensorValue(payload, sensorType.longitude);
  return latitude !== null && longitude !== null && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180
    ? { latitude, longitude } : null;
}

function firmwareVersionOf(row?: Telemetry) {
  if (!row) return "";
  try {
    const value = (JSON.parse(row.payload) as { firmwareVersion?: unknown }).firmwareVersion;
    return typeof value === "string" ? value : "";
  } catch { return ""; }
}

const appConfig = Constants.expoConfig?.extra as AppConfig | undefined;
if (!appConfig) throw new Error("Expo Appwrite configuration is missing");
const config: AppConfig = {
  ...appConfig,
  geofenceAreaTableId: appConfig.geofenceAreaTableId || "geofence-areas",
  geofenceRuleTableId: appConfig.geofenceRuleTableId || "geofence-rules",
  geofenceAlarmTableId: appConfig.geofenceAlarmTableId || "geofence-alarms",
  otaUpdateTableId: appConfig.otaUpdateTableId || "ota-updates",
};
const endpoint = config.appwriteEndpoint.replace(/\/+$/, "");
const otaRepositoryUrl = (config.otaRepositoryUrl || "").replace(/\.git$/, "").replace(/\/$/, "");
const otaImageUrl = /^https:\/\/github\.com\/[^/]+\/[^/]+$/.test(otaRepositoryUrl)
  ? `${otaRepositoryUrl}/releases/latest/download/live-stocking-ota.bin` : "";
const cachePrefix = `live-stocking:${config.appwriteProjectId}:`;
const lastUserCacheKey = `${cachePrefix}last-user`;
const pendingSignOutKey = `${cachePrefix}pending-sign-out`;
const snapshotCacheKey = (userId: string) => `${cachePrefix}snapshot:${userId}`;
const selectedFarmCacheKey = (userId: string) => `${cachePrefix}selected-farm:${userId}`;
const provisioningPop = "abcd1234";
const client = new Client().setEndpoint(endpoint).setProject(config.appwriteProjectId).setPlatform(config.appwritePlatform);
const account = new Account(client);
const tables = new TablesDB(client);
const teams = new Teams(client);
const historyRanges: { key: HistoryRange; label: string; duration: number }[] = [
  { key: "30m", label: "30 MIN", duration: 30 * 60 * 1000 },
  { key: "1h", label: "1 HOUR", duration: 60 * 60 * 1000 },
  { key: "6h", label: "6 HOURS", duration: 6 * 60 * 60 * 1000 },
  { key: "24h", label: "24 HOURS", duration: 24 * 60 * 60 * 1000 },
];
const topologyRecentMs = 2 * 60 * 1000;

function isAuthError(error: unknown) {
  const code = (error as { code?: unknown; status?: unknown } | null)?.code ?? (error as { status?: unknown } | null)?.status;
  return code === 401;
}

async function readCachedSnapshot(userId?: string) {
  try {
    const id = userId ?? await AsyncStorage.getItem(lastUserCacheKey);
    if (!id) return null;
    const [raw, selectedFarmId] = await AsyncStorage.multiGet([snapshotCacheKey(id), selectedFarmCacheKey(id)]);
    const parsed = raw[1] ? JSON.parse(raw[1]) as CachedSnapshot : null;
    if (!parsed || parsed.version !== 1 || parsed.user?.$id !== id || !Array.isArray(parsed.farms) || !Array.isArray(parsed.devices) || !Array.isArray(parsed.telemetry)) return null;
    return { ...parsed, selectedFarmId: selectedFarmId[1] || (parsed.user.prefs as { currentFarmId?: string }).currentFarmId || "" };
  } catch { return null; }
}

async function cacheSnapshot(user: CurrentUser, farms: Farm[], devices: Device[], telemetry: Telemetry[]) {
  const safeFarms: CachedFarm[] = farms.map(({ $id, name, country, location, halowChannel, meshId, teamId, ownerId }) =>
    ({ $id, name, country, location, halowChannel, meshId, teamId, ownerId }));
  const safeDevices: Device[] = devices.map(({ $id, serial, name, status, enabled, metadata }) =>
    ({ $id, serial, name, status, enabled, metadata: { farmId: metadata?.farmId, icon: metadata?.icon, markerColor: metadata?.markerColor } }));
  const safeTelemetry: CachedTelemetry[] = telemetry.map(({ $id, deviceId, serial, channel, topic, payload, receivedAt }) =>
    ({ $id, deviceId, serial, channel, topic, payload, receivedAt }));
  const safeUser: CurrentUser = { $id: user.$id, email: user.email, name: user.name, prefs: { currentFarmId: (user.prefs as { currentFarmId?: string }).currentFarmId } };
  try {
    await AsyncStorage.multiSet([
      [snapshotCacheKey(user.$id), JSON.stringify({ version: 1, user: safeUser, farms: safeFarms, devices: safeDevices, telemetry: safeTelemetry } satisfies CachedSnapshot)],
      [lastUserCacheKey, user.$id],
    ]);
  } catch { /* Offline cache is best effort; live data remains available. */ }
}

async function cacheSelectedFarm(userId: string, farmId: string) {
  try { await AsyncStorage.setItem(selectedFarmCacheKey(userId), farmId); } catch { /* Keep the in-memory selection. */ }
}

async function clearCachedUser(userId: string) {
  try {
    await AsyncStorage.multiRemove([snapshotCacheKey(userId), selectedFarmCacheKey(userId), lastUserCacheKey]);
  } catch { /* The server session has still been removed. */ }
}

function messageOf(error: unknown) { return error instanceof Error ? error.message : "Something went wrong."; }

function voltageOf(row: Telemetry) {
  try {
    const value = sensorValue(JSON.parse(row.payload) as SensorPayload, sensorType.batteryVoltage);
    return (row.channel === "status" || row.channel === "battery") && value !== null && value >= 2.5 && value <= 5.0 ? value : null;
  } catch { return null; }
}

function cachedVoltagePoints(rows: Telemetry[], deviceId: string, duration: number): VoltagePoint[] {
  const since = Date.now() - duration;
  return rows.filter((row) => row.deviceId === deviceId && new Date(row.receivedAt).getTime() >= since)
    .flatMap((row) => {
      const value = voltageOf(row);
      return value === null ? [] : [{ timestamp: new Date(row.receivedAt).getTime(), value }];
    }).sort((a, b) => a.timestamp - b.timestamp);
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

function VoltageChart({ points, duration }: { points: VoltagePoint[]; duration: number }) {
  const [width, setWidth] = useState(0);
  const height = 210;
  const inset = 18;
  const sampled = useMemo(() => {
    if (points.length <= 180) return points;
    const stride = Math.ceil(points.length / 180);
    return points.filter((_, index) => index % stride === 0 || index === points.length - 1);
  }, [points]);
  const values = points.map((point) => point.value);
  const rawMin = values.length ? Math.min(...values) : 0;
  const rawMax = values.length ? Math.max(...values) : 0;
  const padding = Math.max(0.05, (rawMax - rawMin) * .15);
  const min = rawMin - padding;
  const max = rawMax + padding;
  const end = Date.now();
  const start = end - duration;
  const plotWidth = Math.max(0, width - inset * 2);
  const plotHeight = height - inset * 2;
  const coordinates = sampled.map((point) => ({
    x: inset + Math.max(0, Math.min(1, (point.timestamp - start) / duration)) * plotWidth,
    y: inset + (1 - (point.value - min) / (max - min)) * plotHeight,
  }));

  return <View style={styles.chart} onLayout={(event: LayoutChangeEvent) => setWidth(event.nativeEvent.layout.width)}>
    {[0, 1, 2, 3].map((line) => <View key={line} style={[styles.chartGrid, { top: inset + line * plotHeight / 3 }]} />)}
    {width > 0 && coordinates.slice(1).map((point, index) => {
      const previous = coordinates[index];
      const length = Math.hypot(point.x - previous.x, point.y - previous.y);
      const angle = Math.atan2(point.y - previous.y, point.x - previous.x);
      return <View key={`${sampled[index + 1].timestamp}-${index}`} style={[styles.chartLine, { left: (previous.x + point.x) / 2 - length / 2, top: (previous.y + point.y) / 2 - 1, width: length, transform: [{ rotateZ: `${angle}rad` }] }]} />;
    })}
    {coordinates.length ? <View style={[styles.chartDot, { left: coordinates[coordinates.length - 1].x - 4, top: coordinates[coordinates.length - 1].y - 4 }]} /> : null}
    {!points.length ? <Text style={styles.chartEmpty}>No battery voltage data in this range.</Text> : null}
    {points.length ? <><Text style={styles.chartMax}>{rawMax.toFixed(2)} V</Text><Text style={styles.chartMin}>{rawMin.toFixed(2)} V</Text></> : null}
  </View>;
}

function TopologyCard({ device, links }: { device: Device; links: TopologyLink[] }) {
  const neighbors = links.map((link) => ({
    id: link.gatewayDeviceId === device.$id ? link.peerDeviceId : link.gatewayDeviceId,
    serial: link.gatewayDeviceId === device.$id ? link.peerSerial : link.gatewaySerial,
    radioMac: link.peerRadioMac,
    rssi: link.rssi,
    lastSeenAt: link.lastSeenAt,
  })).filter((neighbor, index, items) => items.findIndex((item) => item.id === neighbor.id) === index);
  const updatedAt = links.reduce((latest, link) => link.reportedAt > latest ? link.reportedAt : latest, "");
  return <View style={styles.topologyCard}>
    <View style={styles.topologyHeader}><View><Text style={styles.chartTitle}>Mesh topology</Text><Text style={styles.chartSubtitle}>Direct HaLow links from the last 2 minutes</Text></View><Text style={styles.topologyCount}>{neighbors.length} LINKS{updatedAt ? ` · ${relativeTime(updatedAt)}` : ""}</Text></View>
    <View style={styles.topologyRoot}><View style={styles.topologyRootIcon}><MaterialCommunityIcons name="router-wireless" size={20} color="white" /></View><View><Text style={styles.topologyNodeName}>{device.name}</Text><Text style={styles.topologyNodeMeta}>{device.serial}</Text></View></View>
    {neighbors.map((neighbor) => <View key={neighbor.id} style={styles.topologyLinkRow}>
      <View style={styles.topologyRail}><View style={styles.topologyVertical} /><View style={styles.topologyHorizontal} /><View style={styles.topologyPeerDot} /></View>
      <View style={styles.topologyPeerInfo}><Text style={styles.topologyNodeName}>{neighbor.serial}</Text><Text style={styles.topologyNodeMeta}>{neighbor.radioMac} · {typeof neighbor.rssi === "number" ? `${neighbor.rssi} dBm` : "direct link"} · {relativeTime(neighbor.lastSeenAt)}</Text></View>
    </View>)}
    {!neighbors.length ? <Text style={styles.topologyEmpty}>No active mesh links reported for this device.</Text> : null}
  </View>;
}

function FarmFields({ value, onChange, onPickLocation }: { value: FarmDetails; onChange: (value: FarmDetails) => void; onPickLocation: () => void }) {
  const set = (key: keyof FarmDetails, text: string) => onChange({ ...value, [key]: text });
  const [openDropdown, setOpenDropdown] = useState<"country" | "channel" | null>(null);
  const [passphraseVisible, setPassphraseVisible] = useState(false);
  const channels = channelsForCountry(value.country);
  const selectedCountry = halowCountries.find((country) => country.code === value.country);
  const selectedChannel = channels.find((channel) => String(channel.number) === value.halowChannel);
  return <>
    <Text style={styles.fieldLabel}>FARM NAME</Text>
    <TextInput style={styles.inputLight} value={value.name} onChangeText={(text) => set("name", text)} placeholder="Farm name" maxLength={128} />
    <Text style={styles.fieldLabel}>COUNTRY</Text>
    <Pressable style={styles.selectField} onPress={() => setOpenDropdown(openDropdown === "country" ? null : "country")} accessibilityRole="button" accessibilityState={{ expanded: openDropdown === "country" }}><Text style={styles.selectText}>{selectedCountry ? `${selectedCountry.name} (${selectedCountry.code})` : "Choose country"}</Text><Text style={styles.selectText}>⌄</Text></Pressable>
    {openDropdown === "country" && <View style={styles.optionList}>{halowCountries.map((country) => <Pressable key={country.code} style={styles.optionRow} onPress={() => { onChange({ ...value, country: country.code, halowChannel: String(centerChannelForCountry(country.code)?.number ?? "") }); setOpenDropdown(null); }}><Text style={styles.selectText}>{country.name} ({country.code})</Text></Pressable>)}</View>}
    <Text style={styles.fieldLabel}>LOCATION</Text>
    <Pressable style={styles.selectField} onPress={onPickLocation} accessibilityRole="button"><Text style={styles.selectText}>{value.location || "Choose on offline map"}</Text><Text style={styles.selectText}>⌖</Text></Pressable>
    <Text style={styles.fieldLabel}>HALOW CHANNEL · 1 MHz</Text>
    <Pressable style={styles.selectField} onPress={() => value.country && setOpenDropdown(openDropdown === "channel" ? null : "channel")} accessibilityRole="button" accessibilityState={{ expanded: openDropdown === "channel" }}><Text style={styles.selectText}>{selectedChannel ? `Channel ${selectedChannel.number} · ${selectedChannel.frequencyMHz} MHz` : "Choose country first"}</Text><Text style={styles.selectText}>⌄</Text></Pressable>
    {openDropdown === "channel" && <View style={styles.optionList}>{channels.map((channel) => <Pressable key={channel.number} style={styles.optionRow} onPress={() => { set("halowChannel", String(channel.number)); setOpenDropdown(null); }}><Text style={styles.selectText}>Channel {channel.number} · {channel.frequencyMHz} MHz</Text></Pressable>)}</View>}
    <Text style={styles.fieldLabel}>MESH ID</Text>
    <TextInput style={styles.inputLight} value={value.meshId} onChangeText={(text) => set("meshId", text)} placeholder="Mesh ID" maxLength={32} autoCapitalize="none" />
    <Text style={styles.fieldLabel}>MESH PASSPHRASE</Text>
    <View style={styles.passphraseField}>
      <TextInput style={styles.passphraseInput} value={value.meshPassphrase} onChangeText={(text) => set("meshPassphrase", text)} placeholder="At least 8 characters" secureTextEntry={!passphraseVisible} maxLength={63} autoCapitalize="none" />
      <Pressable style={styles.visibilityButton} onPress={() => setPassphraseVisible((visible) => !visible)} accessibilityRole="button" accessibilityLabel={passphraseVisible ? "Hide mesh passphrase" : "Show mesh passphrase"} accessibilityState={{ checked: passphraseVisible }}>
        <Ionicons name={passphraseVisible ? "eye-outline" : "eye-off-outline"} size={24} color="#59716f" />
      </Pressable>
    </View>
  </>;
}

const countryMapCenters: Record<string, { latitude: number; longitude: number }> = {
  AU: { latitude: -25.2744, longitude: 133.7751 }, CA: { latitude: 56.1304, longitude: -106.3468 },
  EU: { latitude: 50.8503, longitude: 4.3517 }, GB: { latitude: 54.0, longitude: -2.0 },
  IN: { latitude: 20.5937, longitude: 78.9629 }, JP: { latitude: 36.2048, longitude: 138.2529 },
  KR: { latitude: 35.9078, longitude: 127.7669 }, NZ: { latitude: -40.9006, longitude: 174.886 },
  US: { latitude: 39.8283, longitude: -98.5795 },
};

function FarmLocationPicker({ location, country, device = false, onCancel, onSelect }: { location: string; country: string; device?: boolean; onCancel: () => void; onSelect: (location: string) => void }) {
  const map = useRef<EdgezOrganicMapRef>(null);
  const initial = useMemo(() => coordinatesFromLocation(location) ?? countryMapCenters[country] ?? { latitude: 59.3293, longitude: 18.0686 }, [location, country]);
  const [center, setCenter] = useState(initial);
  const [region, setRegion] = useState("");
  const [mapError, setMapError] = useState("");
  return <SafeAreaView style={styles.dialogPage}>
    <View style={styles.dialogHeader}><View><Text style={styles.stepLabel}>{device ? "DEVICE LOCATION" : "FARM LOCATION"}</Text><Text style={styles.dialogTitle}>Choose on map</Text></View><Pressable onPress={onCancel}><Text style={styles.close}>BACK</Text></Pressable></View>
    <View style={styles.locationMap}>
      <EdgezOrganicMap ref={map} nodes={[]} centerLatitude={initial.latitude} centerLongitude={initial.longitude} zoom={9} enableMapDownloads style={styles.map} onMapReady={() => map.current?.getCamera()} onCameraChanged={({ latitude, longitude }) => setCenter({ latitude, longitude })} onMapRegionAvailable={setRegion} onMapError={setMapError} />
      <View pointerEvents="none" style={styles.mapCrosshair}><Text style={styles.mapCrosshairText}>＋</Text></View>
      {region && <View style={styles.mapPrompt}><Text style={styles.mapPromptTitle}>Save {region} offline?</Text><View style={styles.mapPromptActions}><Pressable style={styles.mapDownloadButton} onPress={() => { map.current?.downloadRegion(region); setRegion(""); }}><Text style={styles.mapDownloadText}>DOWNLOAD</Text></Pressable><Pressable style={styles.mapLaterButton} onPress={() => { map.current?.dismissDownloadRegion(region); setRegion(""); }}><Text style={styles.mapLaterText}>NOT NOW</Text></Pressable></View></View>}
      {mapError && <View style={[styles.mapNotice, styles.mapError]}><Text style={styles.mapNoticeText}>{mapError}</Text></View>}
    </View>
    <View style={styles.locationFooter}><Text style={styles.dialogHelp}>Pan the offline map until the crosshair marks your {device ? "device" : "farm"}.</Text><Text style={styles.muted}>{center.latitude.toFixed(6)}, {center.longitude.toFixed(6)}</Text><Pressable style={styles.primary} onPress={() => onSelect(`${center.latitude.toFixed(6)}, ${center.longitude.toFixed(6)}`)}><Text style={styles.primaryText}>USE THIS LOCATION</Text></Pressable></View>
  </SafeAreaView>;
}

function AreaMapEditor({ draft, country, onCancel, onSave }: { draft: AreaDraft; country: string; onCancel: () => void; onSave: (draft: AreaDraft) => void }) {
  const map = useRef<EdgezOrganicMapRef>(null);
  const initial = useMemo(() => coordinatesFromLocation(draft.location) ?? countryMapCenters[country] ?? { latitude: 59.3293, longitude: 18.0686 }, [draft.location, country]);
  const [center, setCenter] = useState(initial);
  const [primary, setPrimary] = useState(Number(draft.primary) || 100);
  const [secondary, setSecondary] = useState(Number(draft.secondary) || 100);
  const [vertices, setVertices] = useState(draft.vertices);
  const resize = (setter: React.Dispatch<React.SetStateAction<number>>, amount: number) => setter((value) => Math.max(10, Math.min(100000, value + amount)));
  const polygonVertices = vertices.split(";").map((vertex) => coordinatesFromLocation(vertex)).filter((vertex): vertex is { latitude: number; longitude: number } => Boolean(vertex));
  const vertexCount = polygonVertices.length;
  const distinctVertexCount = new Set(polygonVertices.map((point) => `${point.latitude.toFixed(6)},${point.longitude.toFixed(6)}`)).size;
  const addVertex = () => setVertices((value) => [...value.split(";").filter(Boolean), `${center.latitude.toFixed(6)},${center.longitude.toFixed(6)}`].join(";"));
  const removeVertex = () => setVertices((value) => value.split(";").filter(Boolean).slice(0, -1).join(";"));
  const boundaryPoints = geofenceBoundaryPoints(draft.shape, {
    center,
    radiusMeters: primary,
    radiusXMeters: primary,
    radiusYMeters: secondary,
    widthMeters: primary,
    heightMeters: secondary,
    vertices: polygonVertices,
  });
  const previewLines: EdgezMapLine[] = boundaryPoints.length >= 2 ? [{
    id: "area-preview",
    points: boundaryPoints.length >= 3 ? [...boundaryPoints, boundaryPoints[0]] : boundaryPoints,
    color: areaColor(draft.color),
  }] : [];
  return <SafeAreaView style={styles.dialogPage}>
    <View style={styles.dialogHeader}><View><Text style={styles.stepLabel}>EDIT AREA ON MAP</Text><Text style={styles.dialogTitle}>{draft.name || "New area"}</Text></View><Pressable onPress={onCancel}><Text style={styles.close}>BACK</Text></Pressable></View>
    <View style={styles.locationMap}>
      <EdgezOrganicMap ref={map} nodes={[]} lines={previewLines} centerLatitude={initial.latitude} centerLongitude={initial.longitude} zoom={16} enableMapDownloads style={styles.map} onMapReady={() => map.current?.getCamera()} onCameraChanged={(nextCamera) => setCenter({ latitude: nextCamera.latitude, longitude: nextCamera.longitude })} />
      <View pointerEvents="none" style={styles.mapCrosshair}><Text style={styles.mapCrosshairText}>＋</Text></View>
      <View style={styles.areaMapControls}>
        {draft.shape === "circle" && <View style={styles.areaDimension}><Text style={styles.areaControlLabel}>RADIUS · {primary} m</Text><View style={styles.areaControlButtons}><Pressable style={styles.areaControl} onPress={() => resize(setPrimary, -10)}><Text style={styles.areaControlText}>−</Text></Pressable><Pressable style={styles.areaControl} onPress={() => resize(setPrimary, 10)}><Text style={styles.areaControlText}>+</Text></Pressable></View></View>}
        {draft.shape === "oval" && <><View style={styles.areaDimension}><Text style={styles.areaControlLabel}>HORIZONTAL · {primary} m</Text><View style={styles.areaControlButtons}><Pressable style={styles.areaControl} onPress={() => resize(setPrimary, -10)}><Text style={styles.areaControlText}>−</Text></Pressable><Pressable style={styles.areaControl} onPress={() => resize(setPrimary, 10)}><Text style={styles.areaControlText}>+</Text></Pressable></View></View><View style={styles.areaDimension}><Text style={styles.areaControlLabel}>VERTICAL · {secondary} m</Text><View style={styles.areaControlButtons}><Pressable style={styles.areaControl} onPress={() => resize(setSecondary, -10)}><Text style={styles.areaControlText}>−</Text></Pressable><Pressable style={styles.areaControl} onPress={() => resize(setSecondary, 10)}><Text style={styles.areaControlText}>+</Text></Pressable></View></View></>}
        {draft.shape === "rectangle" && <><View style={styles.areaDimension}><Text style={styles.areaControlLabel}>WIDTH · {primary} m</Text><View style={styles.areaControlButtons}><Pressable style={styles.areaControl} onPress={() => resize(setPrimary, -10)}><Text style={styles.areaControlText}>−</Text></Pressable><Pressable style={styles.areaControl} onPress={() => resize(setPrimary, 10)}><Text style={styles.areaControlText}>+</Text></Pressable></View></View><View style={styles.areaDimension}><Text style={styles.areaControlLabel}>HEIGHT · {secondary} m</Text><View style={styles.areaControlButtons}><Pressable style={styles.areaControl} onPress={() => resize(setSecondary, -10)}><Text style={styles.areaControlText}>−</Text></Pressable><Pressable style={styles.areaControl} onPress={() => resize(setSecondary, 10)}><Text style={styles.areaControlText}>+</Text></Pressable></View></View></>}
        {draft.shape === "polygon" && <View style={styles.areaDimension}><Text style={styles.areaControlLabel}>POINTS · {vertexCount}</Text><View style={styles.areaControlButtons}><Pressable style={styles.areaControl} onPress={removeVertex} disabled={!vertexCount} accessibilityLabel="Undo last point"><Text style={styles.areaControlText}>↶</Text></Pressable><Pressable style={styles.areaControl} onPress={addVertex} accessibilityLabel="Add point at crosshair"><Text style={styles.areaControlText}>+</Text></Pressable></View></View>}
      </View>
    </View>
    <View style={styles.locationFooter}><Text style={styles.dialogHelp}>{draft.shape === "polygon" ? "Pan until the crosshair marks a corner, then tap +. Add at least three corners; ↶ removes the last one." : "Pan the map to position the area and use the controls to set its dimensions."}</Text><Text style={styles.muted}>{center.latitude.toFixed(6)}, {center.longitude.toFixed(6)}</Text>{draft.shape === "polygon" && <Text style={styles.muted}>{vertexCount} polygon points</Text>}<Pressable style={[styles.primary, draft.shape === "polygon" && distinctVertexCount < 3 && styles.disabledButton]} disabled={draft.shape === "polygon" && distinctVertexCount < 3} onPress={() => onSave({ ...draft, location: `${center.latitude.toFixed(6)}, ${center.longitude.toFixed(6)}`, primary: String(primary), secondary: String(secondary), vertices })}><Text style={styles.primaryText}>USE AREA POSITION</Text></Pressable></View>
  </SafeAreaView>;
}

function OfflineMap({ devices, telemetry, location, areas = [] }: { devices: Device[]; telemetry: Telemetry[]; location?: string; areas?: GeofenceArea[] }) {
  const map = useRef<EdgezOrganicMapRef>(null);
  const [region, setRegion] = useState("");
  const [download, setDownload] = useState<EdgezMapDownloadUpdate | null>(null);
  const [mapError, setMapError] = useState("");
  const farmCenter = coordinatesFromLocation(location ?? "");
  const lines = useMemo<EdgezMapLine[]>(() => areas.map(geofenceLine).filter((line): line is EdgezMapLine => line !== null), [areas]);
  const markers = useMemo<EdgezMapNode[]>(() =>
    devices.flatMap((device) => {
      const coordinates = telemetry.map((row) => row.deviceId === device.$id ? telemetryCoordinates(row) : null).find(Boolean);
      return coordinates ? [{ id: device.$id, label: device.name, ...coordinates, marker: colorForDevice(device), icon: device.metadata?.icon }] : [];
    }), [devices, telemetry]);

  return <View style={styles.mapCard}>
    <EdgezOrganicMap
      ref={map}
      nodes={markers}
      lines={lines}
      centerLatitude={farmCenter?.latitude ?? 59.3293}
      centerLongitude={farmCenter?.longitude ?? 18.0686}
      zoom={farmCenter ? 12 : 9}
      enableMapDownloads
      style={styles.map}
      onMapRegionAvailable={setRegion}
      onMapDownloadUpdate={(update) => { setRegion(""); setDownload(update); }}
      onMapError={setMapError}
    />
    {region ? <View style={styles.mapPrompt}>
      <Text style={styles.mapPromptTitle}>Save {region} offline?</Text>
      <Text style={styles.mapPromptText}>Download this region so the map keeps working without internet.</Text>
      <View style={styles.mapPromptActions}>
        <Pressable style={styles.mapDownloadButton} onPress={() => { map.current?.downloadRegion(region); setRegion(""); }}><Text style={styles.mapDownloadText}>DOWNLOAD</Text></Pressable>
        <Pressable style={styles.mapLaterButton} onPress={() => { map.current?.dismissDownloadRegion(region); setRegion(""); }}><Text style={styles.mapLaterText}>NOT NOW</Text></Pressable>
      </View>
    </View> : null}
    {download && !download.finished ? <View style={styles.mapNotice}><Text style={styles.mapNoticeText}>{download.status}{download.progress === undefined ? "" : ` · ${Math.round(download.progress)}%`}</Text></View> : null}
    {mapError ? <View style={[styles.mapNotice, styles.mapError]}><Text style={styles.mapNoticeText}>{mapError}</Text></View> : null}
    <Text style={styles.mapAttribution}>{markers.length} located devices · {areas.length} areas · © OpenStreetMap contributors</Text>
  </View>;
}

function serialFromBleName(name: string) {
  const serial = /^(PROV_|NRF_)/i.test(name) ? name.slice(name.indexOf("_") + 1).toUpperCase() : "";
  if (!/^[A-F0-9]{12}$/.test(serial)) throw new Error(`Invalid provisioning name: ${name}`);
  return serial;
}

function beaconName(value: string) {
  let result = "";
  let bytes = 0;
  for (const character of value) {
    const encoded = encodeURIComponent(character);
    const width = encoded.startsWith("%") ? encoded.length / 3 : 1;
    if (bytes + width > 64) break;
    result += character;
    bytes += width;
  }
  return result;
}

async function requestBlePermissions() {
  if (Platform.OS !== "android") return;
  const permissions = Number(Platform.Version) >= 31
    ? [
        PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
      ]
    : [PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION];
  const checks = await Promise.all(permissions.map(async (permission) => ({
    permission,
    granted: await PermissionsAndroid.check(permission),
  })));
  const missing = checks.filter(({ granted }) => !granted).map(({ permission }) => permission);
  if (!missing.length) return;

  const results = await PermissionsAndroid.requestMultiple(missing);
  if (missing.some((permission) => results[permission] !== PermissionsAndroid.RESULTS.GRANTED)) {
    throw new Error("Bluetooth permission is required to discover and provision devices.");
  }
}

async function deviceApi<T>(path = "", method: "GET" | "POST" | "PATCH" | "DELETE" = "GET", body?: object) {
  const response = await fetch(`${endpoint}/devices${path}`, {
    method,
    credentials: "include",
    headers: { "content-type": "application/json", "x-appwrite-project": config.appwriteProjectId },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (response.status === 204) return undefined as T;
  const payload = await response.json() as T & { message?: string };
  if (!response.ok) throw Object.assign(new Error(payload.message || "Appwrite Devices request failed."), { code: response.status });
  return payload;
}

export default function App() {
  const [user, setUser] = useState<CurrentUser | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [farms, setFarms] = useState<Farm[]>([]);
  const [currentFarmId, setCurrentFarmId] = useState("");
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [farmManagerOpen, setFarmManagerOpen] = useState(false);
  const [alarmsOpen, setAlarmsOpen] = useState(false);
  const [settingsTab, setSettingsTab] = useState<SettingsTab>("team");
  const [locationPickerOpen, setLocationPickerOpen] = useState(false);
  const [farmFormMode, setFarmFormMode] = useState<"create" | "edit" | null>(null);
  const [farmDraft, setFarmDraft] = useState<FarmDetails>(emptyFarmDetails);
  const [members, setMembers] = useState<Models.Membership[]>([]);
  const [membersLoading, setMembersLoading] = useState(false);
  const [teamError, setTeamError] = useState("");
  const [profileName, setProfileName] = useState("");
  const [inviteName, setInviteName] = useState("");
  const [inviteEmail, setInviteEmail] = useState("");
  const [telemetry, setTelemetry] = useState<Telemetry[]>([]);
  const [topology, setTopology] = useState<TopologyLink[]>([]);
  const [otaUpdates, setOtaUpdates] = useState<OtaUpdate[]>([]);
  const [geofenceAreas, setGeofenceAreas] = useState<GeofenceArea[]>([]);
  const [geofenceRules, setGeofenceRules] = useState<GeofenceRule[]>([]);
  const [geofenceAlarms, setGeofenceAlarms] = useState<GeofenceAlarm[]>([]);
  const [areaDraft, setAreaDraft] = useState<AreaDraft>(emptyAreaDraft);
  const [areaEditingId, setAreaEditingId] = useState<string | null>(null);
  const [areaPickerOpen, setAreaPickerOpen] = useState(false);
  const [ruleName, setRuleName] = useState("");
  const [ruleAreaId, setRuleAreaId] = useState("");
  const [ruleDeviceIds, setRuleDeviceIds] = useState<string[]>([]);
  const [ruleEnter, setRuleEnter] = useState(true);
  const [ruleExit, setRuleExit] = useState(false);
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [bleDevices, setBleDevices] = useState<ProvisioningDevice[]>([]);
  const [selectedBleDevice, setSelectedBleDevice] = useState<ProvisioningDevice | null>(null);
  const [proofOfPossession, setProofOfPossession] = useState(provisioningPop);
  const [bleConnected, setBleConnected] = useState(false);
  const [name, setName] = useState("");
  const [deviceIcon, setDeviceIcon] = useState<EdgezMapIcon>("tracker");
  const [deviceColor, setDeviceColor] = useState<MapMarkerColor>("blue");
  const [deviceLocationChoice, setDeviceLocationChoice] = useState<DeviceLocationChoice>("none");
  const [deviceLocation, setDeviceLocation] = useState("");
  const [deviceLocationPickerOpen, setDeviceLocationPickerOpen] = useState(false);
  const [useUpstreamWifi, setUseUpstreamWifi] = useState<boolean | null>(null);
  const [upstreamSsid, setUpstreamSsid] = useState("");
  const [upstreamPassword, setUpstreamPassword] = useState("");
  const [upstreamNetworks, setUpstreamNetworks] = useState<ESPWifiList[]>([]);
  const [provisioningStatus, setProvisioningStatus] = useState("");
  const [provisioningDialogOpen, setProvisioningDialogOpen] = useState(false);
  const [provisioningStep, setProvisioningStep] = useState<1 | 2 | 3 | 4>(1);
  const [detailDevice, setDetailDevice] = useState<Device | null>(null);
  const [detailIcon, setDetailIcon] = useState<EdgezMapIcon>("tracker");
  const [detailColor, setDetailColor] = useState<MapMarkerColor>("blue");
  const [historyRange, setHistoryRange] = useState<HistoryRange>("1h");
  const [historyPoints, setHistoryPoints] = useState<VoltagePoint[]>([]);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState("");
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState("");
  const [offline, setOffline] = useState(false);
  const [dashboardView, setDashboardView] = useState<DashboardView>("map");
  const [menuOpen, setMenuOpen] = useState(false);
  const activeUserId = useRef<string | null>(null);
  const farmsRef = useRef<Farm[]>([]);
  const devicesRef = useRef<Device[]>([]);
  const telemetryRef = useRef<Telemetry[]>([]);
  const refreshInFlight = useRef<Promise<Farm[]> | null>(null);
  const refreshUserId = useRef<string | null>(null);

  const refresh = useCallback((current: CurrentUser): Promise<Farm[]> => {
    if (refreshInFlight.current && refreshUserId.current === current.$id) return refreshInFlight.current;
    const request = (async () => {
      const [farmResult, deviceResult, telemetryResult, topologyResult, otaResult] = await Promise.allSettled([
        tables.listRows<Farm>({ databaseId: config.databaseId, tableId: config.farmTableId, queries: [Query.limit(100)] }),
        deviceApi<{ devices: Device[] }>(),
        tables.listRows({ databaseId: config.databaseId, tableId: config.telemetryTableId, queries: [Query.orderDesc("receivedAt"), Query.limit(500)] }),
        tables.listRows({ databaseId: config.databaseId, tableId: config.topologyTableId, queries: [Query.equal("active", true), Query.greaterThanEqual("reportedAt", new Date(Date.now() - topologyRecentMs).toISOString()), Query.orderDesc("reportedAt"), Query.limit(500)] }),
        tables.listRows({ databaseId: config.databaseId, tableId: config.otaUpdateTableId, queries: [Query.orderDesc("reportedAt"), Query.limit(500)] }),
      ]);
      const failures = [farmResult, deviceResult, telemetryResult, topologyResult, otaResult].filter((result): result is PromiseRejectedResult => result.status === "rejected");
      const authFailure = failures.find((failure) => isAuthError(failure.reason));
      if (authFailure) throw authFailure.reason;
      if (activeUserId.current !== current.$id) return farmsRef.current;
      if (farmResult.status === "fulfilled") {
        farmsRef.current = farmResult.value.rows;
        setFarms(farmsRef.current);
        setCurrentFarmId((selected) => farmsRef.current.some((farm) => farm.$id === selected) ? selected : farmsRef.current[0]?.$id || "");
      }
      if (deviceResult.status === "fulfilled") {
        devicesRef.current = deviceResult.value.devices;
        setDevices(devicesRef.current);
      }
      if (telemetryResult.status === "fulfilled") {
        telemetryRef.current = telemetryResult.value.rows as unknown as Telemetry[];
        setTelemetry(telemetryRef.current);
      }
      if (topologyResult.status === "fulfilled") setTopology(topologyResult.value.rows as unknown as TopologyLink[]);
      if (otaResult.status === "fulfilled") setOtaUpdates(otaResult.value.rows as unknown as OtaUpdate[]);
      await cacheSnapshot(current, farmsRef.current, devicesRef.current, telemetryRef.current);
      if (failures.length) {
        setOffline(true);
        throw failures[0].reason;
      }
      setOffline(false);
      return farmsRef.current;
    })();
    refreshInFlight.current = request;
    refreshUserId.current = current.$id;
    const clearRefresh = () => { if (refreshInFlight.current === request) { refreshInFlight.current = null; refreshUserId.current = null; } };
    request.then(clearRefresh, clearRefresh);
    return request;
  }, []);

  useEffect(() => {
    let active = true;
    const showCached = (snapshot: NonNullable<Awaited<ReturnType<typeof readCachedSnapshot>>>) => {
      if (!active) return;
      activeUserId.current = snapshot.user.$id;
      farmsRef.current = snapshot.farms.map((farm) => ({ ...farm, meshPassphrase: "" }) as Farm);
      devicesRef.current = snapshot.devices;
      telemetryRef.current = snapshot.telemetry as Telemetry[];
      setUser(snapshot.user); setFarms(farmsRef.current); setDevices(devicesRef.current); setTelemetry(telemetryRef.current);
      setCurrentFarmId(snapshot.selectedFarmId); setOffline(true); setBusy(false);
    };
    void (async () => {
      if (await AsyncStorage.getItem(pendingSignOutKey).catch(() => null)) {
        if (active) setBusy(false);
        return;
      }
      const cached = await readCachedSnapshot();
      if (cached) showCached(cached);
      try {
        const current = await account.get();
        if (!active) return;
        activeUserId.current = current.$id;
        if (cached && cached.user.$id !== current.$id) await clearCachedUser(cached.user.$id);
        const scoped = cached?.user.$id === current.$id ? cached : await readCachedSnapshot(current.$id);
        if (scoped) {
          showCached(scoped);
          await AsyncStorage.setItem(lastUserCacheKey, current.$id).catch(() => undefined);
        }
        else {
          farmsRef.current = []; devicesRef.current = []; telemetryRef.current = [];
          setFarms([]); setDevices([]); setTelemetry([]); setTopology([]); setOtaUpdates([]);
          setCurrentFarmId((current.prefs as { currentFarmId?: string }).currentFarmId || "");
        }
        setUser(current);
        setProfileName(current.name || "");
        const availableFarms = await refresh(current);
        if (active && !availableFarms.length) setSettingsOpen(true);
      } catch (caught) {
        if (!active) return;
        if (isAuthError(caught)) {
          if (cached) await clearCachedUser(cached.user.$id);
          activeUserId.current = null;
          farmsRef.current = []; devicesRef.current = []; telemetryRef.current = [];
          setUser(null); setFarms([]); setDevices([]); setTelemetry([]); setTopology([]); setOtaUpdates([]); setCurrentFarmId(""); setOffline(false);
        } else if (cached) setOffline(true);
        else setError(messageOf(caught));
      } finally { if (active) setBusy(false); }
    })();
    return () => { active = false; };
  }, [refresh]);
  useEffect(() => {
    if (!user) return;
    const timer = setInterval(() => {
      void refresh(user).catch((caught) => {
        if (isAuthError(caught)) {
          void clearCachedUser(user.$id);
          activeUserId.current = null;
          setUser(null); setFarms([]); setDevices([]); setTelemetry([]); setTopology([]); setOtaUpdates([]); setCurrentFarmId(""); setOffline(false);
        } else setOffline(true);
      });
    }, offline ? 15000 : 5000);
    return () => clearInterval(timer);
  }, [refresh, user, offline]);
  useEffect(() => {
    if (user && farms.some((farm) => farm.$id === currentFarmId)) void cacheSelectedFarm(user.$id, currentFarmId);
  }, [user?.$id, farms, currentFarmId]);
  useEffect(() => {
    if (!detailDevice) return;
    let active = true;
    const duration = historyRanges.find((range) => range.key === historyRange)!.duration;
    if (offline) {
      setHistoryPoints(cachedVoltagePoints(telemetryRef.current, detailDevice.$id, duration));
      setHistoryLoading(false); setHistoryError("");
      return;
    }
    setHistoryLoading(true); setHistoryError(""); setHistoryPoints([]);
    tables.listRows({
      databaseId: config.databaseId,
      tableId: config.telemetryTableId,
      queries: [Query.equal("deviceId", detailDevice.$id), Query.greaterThanEqual("receivedAt", new Date(Date.now() - duration).toISOString()), Query.orderAsc("receivedAt"), Query.limit(5000)],
    }).then((result) => {
      if (!active) return;
      const points = (result.rows as unknown as Telemetry[]).flatMap((row) => {
        const value = voltageOf(row);
        return value === null ? [] : [{ timestamp: new Date(row.receivedAt).getTime(), value }];
      });
      setHistoryPoints(points);
    }).catch((caught) => { if (active) { setHistoryPoints(cachedVoltagePoints(telemetryRef.current, detailDevice.$id, duration)); setHistoryError(messageOf(caught)); } })
      .finally(() => { if (active) setHistoryLoading(false); });
    return () => { active = false; };
  }, [detailDevice, historyRange, offline]);

  async function authenticate(register: boolean) {
    setBusy(true); setError("");
    try {
      if (await AsyncStorage.getItem(pendingSignOutKey)) {
        try { await account.deleteSession({ sessionId: "current" }); }
        catch (caught) { if (!isAuthError(caught)) throw caught; }
        await AsyncStorage.removeItem(pendingSignOutKey);
      }
      if (register) await account.create({ userId: ID.unique(), email, password });
      await account.createEmailPasswordSession({ email, password });
      const current = await account.get();
      activeUserId.current = current.$id;
      setUser(current);
      setProfileName(current.name || "");
      setCurrentFarmId((current.prefs as { currentFarmId?: string }).currentFarmId || "");
      const availableFarms = await refresh(current);
      if (!availableFarms.length) setSettingsOpen(true);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function scanBleDevices() {
    setBusy(true); setError(""); setProvisioningStatus("Scanning for ESP32 and nRF54 devices…");
    try {
      selectedBleDevice?.disconnect();
      setSelectedBleDevice(null); setProofOfPossession(provisioningPop); setBleConnected(false);
      await requestBlePermissions();
      let nrf: NrfProvisioningDevice[] = [];
      let nrfError: unknown;
      try { nrf = await scanNrfProvisioningDevices(); }
      catch (caught) { nrfError = caught; }
      let esp: ESPDevice[] = [];
      try {
        const candidates = await ESPProvisionManager.searchESPDevices("PROV_", ESPTransport.ble, ESPSecurity.secure);
        const nrfNames = new Set(nrf.map((device) => device.name.toLowerCase()));
        esp = candidates.filter((device) =>
          /^PROV_[A-F0-9]{12}$/i.test(device.name) && !nrfNames.has(device.name.toLowerCase()));
      } catch (caught) {
        if (nrfError) throw nrfError;
      }
      const valid: ProvisioningDevice[] = [...esp, ...nrf];
      setBleDevices(valid);
      setProvisioningStatus(valid.length ? "Select a device to provision." : "No provisioning devices found.");
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  function openProvisioningDialog() {
    if (offline) return;
    if (!currentFarmId) { openSettings(); return; }
    setMenuOpen(false);
    selectedBleDevice?.disconnect();
    setBleDevices([]); setSelectedBleDevice(null); setProofOfPossession(provisioningPop); setBleConnected(false); setName(""); setDeviceIcon("tracker"); setDeviceColor("blue"); setDeviceLocationChoice("none"); setDeviceLocation(""); setDeviceLocationPickerOpen(false); setUseUpstreamWifi(null); setUpstreamSsid(""); setUpstreamPassword(""); setUpstreamNetworks([]);
    setError(""); setProvisioningStatus("Start by scanning for a device in provisioning mode.");
    setProvisioningStep(1);
    setProvisioningDialogOpen(true);
  }

  function closeProvisioningDialog() {
    if (busy) return;
    selectedBleDevice?.disconnect();
    setSelectedBleDevice(null); setBleConnected(false);
    setDeviceLocationPickerOpen(false);
    setProvisioningDialogOpen(false);
  }

  function previousProvisioningStep() {
    if (provisioningStep === 4) {
      if (selectedBleDevice && isNrfDevice(selectedBleDevice)) {
        selectedBleDevice.disconnect(); setBleConnected(false); setProvisioningStep(2);
      } else setProvisioningStep(3);
      return;
    }
    if (provisioningStep === 3) {
      selectedBleDevice?.disconnect();
      setBleConnected(false);
      setProvisioningStatus("Confirm the device details and PoP.");
      setProvisioningStep(2);
      return;
    }
    setSelectedBleDevice(null);
    setProvisioningStatus("Select a device to provision.");
    setProvisioningStep(1);
  }

  function selectBleDevice(device: ProvisioningDevice) {
    selectedBleDevice?.disconnect();
    setError("");
    setSelectedBleDevice(device);
    const existing = devices.find((item) => item.serial === serialFromBleName(device.name));
    setDeviceIcon(existing?.metadata?.icon || (isNrfDevice(device) ? "tracker" : "gateway"));
    setDeviceColor(existing ? colorForDevice(existing) : "blue");
    const previousLocation = telemetry.filter((row) => row.deviceId === existing?.$id)
      .map(telemetryCoordinates).find((coordinates) => coordinates !== null) ?? null;
    setDeviceLocationChoice(previousLocation ? "map" : "none");
    setDeviceLocation(previousLocation ? `${previousLocation.latitude.toFixed(6)}, ${previousLocation.longitude.toFixed(6)}` : "");
    setProofOfPossession(provisioningPop);
    setBleConnected(false);
    setProvisioningStatus(`Confirm the details for ${device.name}.`);
    setProvisioningStep(2);
  }

  async function chooseCurrentDeviceLocation() {
    setBusy(true); setError("");
    try {
      const permission = await Location.requestForegroundPermissionsAsync();
      if (permission.status !== "granted") throw new Error("Location permission is needed to use the phone's current location.");
      const position = await Location.getCurrentPositionAsync({ accuracy: Location.Accuracy.Balanced });
      setDeviceLocation(`${position.coords.latitude.toFixed(6)}, ${position.coords.longitude.toFixed(6)}`);
      setDeviceLocationChoice("current");
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function connectForProvisioning() {
    if (!selectedBleDevice || (!isNrfDevice(selectedBleDevice) && !proofOfPossession.trim())) return;
    setBusy(true); setError("");
    try {
      setProvisioningStatus(isNrfDevice(selectedBleDevice) ? `Connecting to ${selectedBleDevice.name}…` : `Authenticating ${selectedBleDevice.name} with the provided PoP…`);
      if (isNrfDevice(selectedBleDevice)) await selectedBleDevice.connect();
      else await selectedBleDevice.connect(proofOfPossession.trim());
      setBleConnected(true);
      if (isNrfDevice(selectedBleDevice)) {
        setUseUpstreamWifi(false);
        setProvisioningStatus("The nRF54 uses HaLow for its upstream connection. Confirm its farm configuration.");
        setProvisioningStep(4);
      } else {
        setProvisioningStatus("Choose whether this device has an upstream Wi-Fi connection.");
        setProvisioningStep(3);
      }
    } catch (caught) {
      selectedBleDevice.disconnect();
      setBleConnected(false);
      setError(messageOf(caught));
      setProvisioningStatus("BLE connection failed. Check the device and retry.");
    } finally { setBusy(false); }
  }

  async function chooseUpstreamWifi(enabled: boolean) {
    setUseUpstreamWifi(enabled);
    setUpstreamSsid(""); setUpstreamPassword(""); setUpstreamNetworks([]);
    if (!enabled || !selectedBleDevice || isNrfDevice(selectedBleDevice)) return;
    setBusy(true); setError(""); setProvisioningStatus("Scanning nearby Wi-Fi networks…");
    try {
      const networks = await selectedBleDevice.scanWifiList();
      setUpstreamNetworks(networks.filter((network) => Boolean(network.ssid)).sort((a, b) => b.rssi - a.rssi));
      setProvisioningStatus("Choose the device's upstream Wi-Fi network.");
    } catch (caught) { setError(messageOf(caught)); setProvisioningStatus("Wi-Fi scan failed. Retry or choose no Wi-Fi."); }
    finally { setBusy(false); }
  }

  async function provisionDevice() {
    const farm = farms.find((candidate) => candidate.$id === currentFarmId);
    if (!user || offline || !farm || !selectedBleDevice || !bleConnected) return;
    setBusy(true); setError(""); setProvisioningStatus("Preparing the device credential…");
    try {
      const serial = serialFromBleName(selectedBleDevice.name);

      setProvisioningStatus("Creating Appwrite device credential…");
      let appwriteDevice = devices.find((device) => device.serial === serial);
      if (appwriteDevice && appwriteDevice.metadata?.farmId !== farm.$id) {
        throw new Error("This device is already assigned to another farm. Open Settings to select that farm.");
      }
      const coordinates = deviceLocationChoice === "none" || deviceLocationChoice === "gps" ? null : coordinatesFromLocation(deviceLocation);
      if (deviceLocationChoice !== "none" && deviceLocationChoice !== "gps" && !coordinates) throw new Error("Choose a valid device location before provisioning.");
      if (!appwriteDevice) {
        appwriteDevice = await deviceApi<Device>("", "POST", {
          serial,
          name: name.trim() || serial,
          enabled: true,
          permissions: [
            Permission.read(Role.team(farm.teamId)),
            Permission.update(Role.team(farm.teamId, "owner")),
            Permission.delete(Role.team(farm.teamId, "owner")),
          ],
          metadata: { farmId: farm.$id, icon: deviceIcon, markerColor: deviceColor, firmwareTarget: isNrfDevice(selectedBleDevice) ? "nrf54l15" : "heltec-hc33" },
        });
      } else if (appwriteDevice.metadata?.icon !== deviceIcon || appwriteDevice.metadata?.markerColor !== deviceColor || !appwriteDevice.metadata?.firmwareTarget) {
        appwriteDevice = await deviceApi<Device>(`/${encodeURIComponent(appwriteDevice.$id)}`, "PATCH", {
          metadata: { ...appwriteDevice.metadata, icon: deviceIcon, markerColor: deviceColor, firmwareTarget: isNrfDevice(selectedBleDevice) ? "nrf54l15" : "heltec-hc33" },
        });
      }
      const mqtt = await deviceApi<Credential>(`/${encodeURIComponent(appwriteDevice.$id)}/credentials`, "POST", {});

      setProvisioningStatus(isNrfDevice(selectedBleDevice)
        ? `Saving ${farm.name} configuration and waiting for NVS confirmation…`
        : `Sending ${farm.name} device configuration…`);
      const payload = JSON.stringify({
        clientId: mqtt.clientId,
        username: mqtt.username,
        password: mqtt.password,
        projectId: config.appwriteProjectId,
        channel: "status",
        meshId: farm.meshId,
        passphrase: farm.meshPassphrase,
        country: farm.country,
        halowChannel: farm.halowChannel,
        wifiUpstream: useUpstreamWifi === true,
        ...(isNrfDevice(selectedBleDevice) ? { halowFrequencyKHz: Math.round((channelsForCountry(farm.country).find((item) => item.number === farm.halowChannel)?.frequencyMHz || 0) * 1000) } : {}),
        ...(isNrfDevice(selectedBleDevice) ? { deviceName: beaconName(name.trim() || serial) } : {}),
        ...(isNrfDevice(selectedBleDevice) ? { useDeviceGps: deviceLocationChoice === "gps" } : {}),
        ...(coordinates ?? (isNrfDevice(selectedBleDevice) ? {} : { latitude: null, longitude: null })),
      });
      const mqttResponse = isNrfDevice(selectedBleDevice)
        ? await selectedBleDevice.sendMqttConfig(payload)
        : await selectedBleDevice.sendData("mqtt-config", payload);
      const accepted = JSON.parse(mqttResponse) as { ok?: boolean; persisted?: boolean; error?: string };
      if (!accepted.ok || (isNrfDevice(selectedBleDevice) && !accepted.persisted)) {
        throw new Error(accepted.error || "The device did not confirm that its configuration was persisted.");
      }
      if (useUpstreamWifi && !isNrfDevice(selectedBleDevice)) {
        setProvisioningStatus("Connecting the ESP32 to upstream Wi-Fi…");
        await selectedBleDevice.provision(upstreamSsid, upstreamPassword);
      }
      setProvisioningStatus(`Provisioned ${serial}.`);
      setSelectedBleDevice(null); setBleDevices([]); setProofOfPossession(provisioningPop); setBleConnected(false); setName(""); setDeviceIcon("tracker"); setDeviceColor("blue"); setDeviceLocationChoice("none"); setDeviceLocation("");
      setProvisioningDialogOpen(false);
      await refresh(user);
    } catch (caught) { setError(messageOf(caught)); setProvisioningStatus("Provisioning did not complete."); }
    finally { selectedBleDevice.disconnect(); setBleConnected(false); setBusy(false); }
  }

  async function saveDeviceAppearance() {
    if (!detailDevice || offline) return;
    setBusy(true); setError("");
    try {
      const updated = await deviceApi<Device>(`/${encodeURIComponent(detailDevice.$id)}`, "PATCH", {
        metadata: { ...detailDevice.metadata, icon: detailIcon, markerColor: detailColor },
      });
      devicesRef.current = devicesRef.current.map((item) => item.$id === updated.$id ? updated : item);
      setDevices(devicesRef.current);
      setDetailDevice(updated);
      if (user) await cacheSnapshot(user, farmsRef.current, devicesRef.current, telemetryRef.current);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  function requestFirmwareUpdate(device: Device) {
    if (otaUpdates.some((update) => update.deviceId === device.$id && update.status === "pending")) {
      setError("An OTA update is already pending for this device.");
      return;
    }
    Alert.alert("Update HT-HC33 firmware", `Install the latest firmware on ${device.name}?`, [
      { text: "Cancel", style: "cancel" },
      { text: "Update", onPress: () => void (async () => {
        if (!otaImageUrl) { setError("This deployment does not expose a supported source repository for OTA."); return; }
        setBusy(true); setError("");
        try {
          await deviceApi(`/${encodeURIComponent(device.$id)}/commands`, "POST", {
            command: "ota", payload: { url: otaImageUrl, requestId: ID.unique() },
          });
          Alert.alert("Update requested", "The device will download, install, and restart in the background.");
        } catch (caught) { setError(messageOf(caught)); }
        finally { setBusy(false); }
      })() },
    ]);
  }

  async function signOut() {
    setMenuOpen(false);
    selectedBleDevice?.disconnect();
    activeUserId.current = null;
    if (offline) await AsyncStorage.setItem(pendingSignOutKey, "1");
    else {
      try {
        await account.deleteSession({ sessionId: "current" });
        await AsyncStorage.removeItem(pendingSignOutKey);
      } catch (caught) {
        if (!isAuthError(caught)) await AsyncStorage.setItem(pendingSignOutKey, "1");
      }
      await refreshInFlight.current?.catch(() => undefined);
    }
    if (user) await clearCachedUser(user.$id);
    farmsRef.current = []; devicesRef.current = []; telemetryRef.current = [];
    setUser(null); setDevices([]); setFarms([]); setCurrentFarmId(""); setSettingsOpen(false); setLocationPickerOpen(false); setFarmFormMode(null); setMembers([]); setTelemetry([]); setTopology([]); setOtaUpdates([]); setBleDevices([]); setSelectedBleDevice(null); setProofOfPossession(provisioningPop); setBleConnected(false); setProvisioningDialogOpen(false); setDetailDevice(null); setDashboardView("map"); setOffline(false);
  }

  function openSettings() {
    setMenuOpen(false);
    setLocationPickerOpen(false);
    setFarmManagerOpen(false);
    setSettingsTab("team");
    setFarmFormMode(null);
    setTeamError("");
    setProfileName(user?.name || "");
    setError("");
    setSettingsOpen(true);
  }

  async function selectFarm(farm: Farm) {
    if (!user) return;
    setBusy(true); setError("");
    try {
      if (!offline) {
        const updated = await account.updatePrefs({ prefs: { ...user.prefs, currentFarmId: farm.$id } });
        setUser(updated);
      }
      setCurrentFarmId(farm.$id);
      await cacheSelectedFarm(user.$id, farm.$id);
      setFarmFormMode(null);
      setInviteName("");
      setInviteEmail("");
      setDetailDevice(null);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function createFarm() {
    if (!user || offline) return;
    setBusy(true); setError("");
    const teamId = ID.unique();
    try {
      const data = farmData(farmDraft);
      await teams.create({ teamId, name: data.name });
      let farm: Farm;
      try {
        farm = await tables.createRow<Farm>({
          databaseId: config.databaseId,
          tableId: config.farmTableId,
          rowId: teamId,
          data: { ...data, teamId, ownerId: user.$id },
          permissions: [
            Permission.read(Role.team(teamId)),
            Permission.update(Role.team(teamId, "owner")),
            Permission.delete(Role.team(teamId, "owner")),
          ],
        });
      } catch (caught) {
        await teams.delete({ teamId }).catch(() => undefined);
        throw caught;
      }
      farmsRef.current = [...farmsRef.current, farm];
      setFarms(farmsRef.current);
      await cacheSnapshot(user, farmsRef.current, devicesRef.current, telemetryRef.current);
      setFarmFormMode(null);
      setFarmDraft(emptyFarmDetails);
      await selectFarm(farm);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function saveFarm() {
    const farm = farms.find((candidate) => candidate.$id === currentFarmId);
    if (!farm || offline || farm.ownerId !== user?.$id) return;
    setBusy(true); setError("");
    try {
      const data = farmData(farmDraft);
      const updated = await tables.updateRow<Farm>({ databaseId: config.databaseId, tableId: config.farmTableId, rowId: farm.$id, data });
      if (data.name !== farm.name) await teams.updateName({ teamId: farm.teamId, name: data.name });
      farmsRef.current = farmsRef.current.map((item) => item.$id === farm.$id ? updated : item);
      setFarms(farmsRef.current);
      await cacheSnapshot(user!, farmsRef.current, devicesRef.current, telemetryRef.current);
      setFarmFormMode(null);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function inviteMember() {
    const farm = farms.find((candidate) => candidate.$id === currentFarmId);
    const address = inviteEmail.trim().toLowerCase();
    const name = inviteName.trim();
    const inviteUrl = (process.env.EXPO_PUBLIC_APPWRITE_TEAM_INVITE_URL || config.teamInviteUrl)?.trim();
    if (!farm || offline || farm.ownerId !== user?.$id) return;
    if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(address)) { setTeamError("Enter a valid email address."); return; }
    if (!inviteUrl) { setTeamError("Invitation link is missing from this app build. Restart Metro or install an updated build."); return; }
    setBusy(true); setTeamError("");
    try {
      await teams.createMembership({ teamId: farm.teamId, roles: [Roles.Developer], email: address, ...(name ? { name } : {}), url: inviteUrl });
      setInviteName("");
      setInviteEmail("");
      setMembers((await teams.listMemberships({ teamId: farm.teamId, queries: [Query.limit(100)] })).memberships);
      Alert.alert("Invitation sent", `An invitation was sent to ${address}.`);
    } catch (caught) { setTeamError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function saveProfileName() {
    const name = profileName.trim();
    if (!user || offline) return;
    if (!name) { setTeamError("Enter your name."); return; }
    setBusy(true); setTeamError("");
    try {
      const updated = await account.updateName({ name });
      setUser(updated);
      await cacheSnapshot(updated, farmsRef.current, devicesRef.current, telemetryRef.current);
      if (currentFarmId) {
        const farm = farms.find((candidate) => candidate.$id === currentFarmId);
        if (farm) setMembers((await teams.listMemberships({ teamId: farm.teamId, queries: [Query.limit(100)] })).memberships);
      }
    } catch (caught) { setTeamError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  function requestRemoveMember(member: Models.Membership) {
    const farm = farms.find((candidate) => candidate.$id === currentFarmId);
    if (!farm || farm.ownerId !== user?.$id || member.roles.includes("owner") || member.userId === user.$id) return;
    Alert.alert(`Remove ${member.userName || member.userEmail}?`, "This person will lose access to this farm and its device readings.", [
      { text: "Cancel", style: "cancel" },
      { text: "Remove", style: "destructive", onPress: () => void removeMember(farm.teamId, member.$id) },
    ]);
  }

  async function removeMember(teamId: string, membershipId: string) {
    const farm = farms.find((candidate) => candidate.$id === currentFarmId);
    const member = members.find((candidate) => candidate.$id === membershipId);
    if (offline || !farm || farm.teamId !== teamId || farm.ownerId !== user?.$id || !member || member.roles.includes("owner") || member.userId === user.$id) return;
    setBusy(true); setTeamError("");
    try {
      await teams.deleteMembership({ teamId, membershipId });
      setMembers((current) => current.filter((member) => member.$id !== membershipId));
    } catch (caught) { setTeamError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  function requestDeleteDevice(device: Device) {
    Alert.alert(
      `Delete ${device.name}?`,
      "This permanently removes the device and its MQTT credentials. Existing telemetry rows are not deleted.",
      [
        { text: "Cancel", style: "cancel" },
        { text: "Delete device", style: "destructive", onPress: () => void deleteSelectedDevice(device) },
      ],
    );
  }

  async function deleteSelectedDevice(device: Device) {
    if (!user || offline) return;
    setBusy(true); setError("");
    try {
      await deviceApi<void>(`/${encodeURIComponent(device.$id)}`, "DELETE");
      setDetailDevice(null);
      await refresh(user);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
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
  const historyStats = useMemo(() => {
    if (!historyPoints.length) return null;
    const values = historyPoints.map((point) => point.value);
    return { min: Math.min(...values), max: Math.max(...values), average: values.reduce((sum, value) => sum + value, 0) / values.length };
  }, [historyPoints]);
  const activeHistoryRange = historyRanges.find((range) => range.key === historyRange)!;
  const activeHistoryDuration = activeHistoryRange.duration;
  const detailLatest = detailDevice ? latestVoltageByDevice.get(detailDevice.$id) : undefined;
  const detailStatus = detailDevice ? statusOf(detailDevice, latestTelemetryByDevice.get(detailDevice.$id)) : "";
  const detailFirmwareVersion = firmwareVersionOf(detailDevice ? latestTelemetryByDevice.get(detailDevice.$id) : undefined);
  const detailTopology = detailDevice ? topology.filter((link) => isRecentTopology(link) && (link.gatewayDeviceId === detailDevice.$id || link.peerDeviceId === detailDevice.$id)) : [];
  const detailOtaUpdate = detailDevice ? otaUpdates.find((update) => update.deviceId === detailDevice.$id) : undefined;
  const detailOtaPending = Boolean(detailDevice && otaUpdates.some((update) => update.deviceId === detailDevice.$id && update.status === "pending"));
  const currentFarm = farms.find((farm) => farm.$id === currentFarmId);
  const activeTeamId = currentFarm?.teamId;
  useEffect(() => {
    if (!settingsOpen || !activeTeamId || offline || !user) { setMembers([]); setMembersLoading(false); return; }
    let active = true;
    setMembers([]); setMembersLoading(true); setTeamError("");
    teams.listMemberships({ teamId: activeTeamId, queries: [Query.limit(100)] })
      .then((result) => { if (active) setMembers(result.memberships); })
      .catch((caught) => { if (active) setTeamError(messageOf(caught)); })
      .finally(() => { if (active) setMembersLoading(false); });
    return () => { active = false; };
  }, [settingsOpen, activeTeamId, offline, user?.$id]);
  useEffect(() => {
    if (!currentFarmId || offline || !config.geofenceAreaTableId || !config.geofenceRuleTableId || !config.geofenceAlarmTableId) {
      setGeofenceAreas([]); setGeofenceRules([]); setGeofenceAlarms([]); return;
    }
    let active = true;
    setGeofenceAreas([]); setGeofenceRules([]); setGeofenceAlarms([]);
    Promise.allSettled([
      tables.listRows<GeofenceArea>({ databaseId: config.databaseId, tableId: config.geofenceAreaTableId, queries: [Query.equal("farmId", currentFarmId), Query.limit(100)] }),
      tables.listRows<GeofenceRule>({ databaseId: config.databaseId, tableId: config.geofenceRuleTableId, queries: [Query.equal("farmId", currentFarmId), Query.limit(100)] }),
      tables.listRows<GeofenceAlarm>({ databaseId: config.databaseId, tableId: config.geofenceAlarmTableId, queries: [Query.equal("farmId", currentFarmId), Query.equal("active", true), Query.limit(100)] }),
    ]).then(([areas, rules, alarms]) => {
      if (!active) return;
      if (areas.status === "fulfilled") setGeofenceAreas(areas.value.rows);
      else setError(`Could not load geofence areas: ${messageOf(areas.reason)}`);
      if (rules.status === "fulfilled") setGeofenceRules(rules.value.rows);
      if (alarms.status === "fulfilled") setGeofenceAlarms(alarms.value.rows);
    });
    return () => { active = false; };
  }, [currentFarmId, offline]);
  const visibleDevices = devices.filter((device) => Boolean(currentFarmId) && device.metadata?.farmId === currentFarmId);
  if (busy && !user) return <SafeAreaProvider><SafeAreaView style={styles.safe}><ActivityIndicator style={styles.loader} color="#62d8cf" size="large" /></SafeAreaView></SafeAreaProvider>;

  return <SafeAreaProvider><SafeAreaView style={styles.safe}><StatusBar style="light" /><KeyboardAvoidingView style={[styles.screen, user && styles.dashboardScreen]} behavior={Platform.OS === "ios" ? "padding" : undefined}>
    {!user ? <><View style={styles.header}><View><Text style={styles.eyebrow}>APPWRITE DEVICES · MQTT</Text><Text style={styles.title}>Operator access</Text></View></View><View style={styles.auth}>
      <Text style={styles.hero}>Devices in.{"\n"}<Text style={styles.accent}>Signals out.</Text></Text>
      <Text style={styles.authLabel}>EMAIL</Text>
      <TextInput style={styles.input} value={email} onChangeText={setEmail} placeholder="Email" placeholderTextColor="#718a83" autoCapitalize="none" keyboardType="email-address" />
      <Text style={styles.authLabel}>PASSWORD</Text>
      <TextInput style={styles.input} value={password} onChangeText={setPassword} placeholder="Password" placeholderTextColor="#718a83" secureTextEntry />
      <Pressable style={styles.primary} onPress={() => authenticate(false)} disabled={busy}><Text style={styles.primaryText}>SIGN IN</Text></Pressable>
      <Pressable style={styles.secondary} onPress={() => authenticate(true)} disabled={busy}><Text style={styles.secondaryText}>CREATE ACCOUNT</Text></Pressable>
    </View></> : <View style={styles.dashboard}>
      {dashboardView === "map" ? <OfflineMap key={`${currentFarm?.$id ?? "none"}:${currentFarm?.location ?? ""}`} devices={visibleDevices} telemetry={telemetry} location={currentFarm?.location} areas={geofenceAreas} /> : <ScrollView style={styles.listScroll} contentContainerStyle={styles.listContent} keyboardShouldPersistTaps="handled">
      <Text style={styles.sectionLabel}>{visibleDevices.length} DEVICES</Text>
      {visibleDevices.map((device) => {
        const latest = latestVoltageByDevice.get(device.$id);
        const status = statusOf(device, latestTelemetryByDevice.get(device.$id));
        return <Pressable key={device.$id} style={styles.deviceCard} onPress={() => { setHistoryRange("1h"); setDetailIcon(device.metadata?.icon || "tracker"); setDetailColor(colorForDevice(device)); setDetailDevice(device); }}>
          <View style={styles.deviceCardHeader}><View><Text style={styles.deviceCardName}>{device.name}</Text><Text style={styles.deviceSerial}>{device.serial}</Text></View><View style={styles.statusBadge}><View style={[styles.statusDot, status === "Online" ? styles.statusOnline : styles.statusOffline]} /><Text style={styles.statusText}>{status.toUpperCase()}</Text></View></View>
          <View style={styles.latestRow}><View><Text style={styles.latestLabel}>LATEST BATTERY VOLTAGE</Text><Text style={styles.latestValue}>{latest ? `${latest.value.toFixed(2)} V` : "—"}</Text></View><Text style={styles.cardArrow}>›</Text></View>
          <Text style={styles.lastSeen}>{latest ? `Updated ${relativeTime(latest.row.receivedAt)}` : "Waiting for battery telemetry"}</Text>
        </Pressable>;
      })}
      {!currentFarm ? <Text style={styles.empty}>Create a farm in Settings before adding devices.</Text> : !visibleDevices.length ? <Text style={styles.empty}>No devices in this farm yet. Use + ADD to provision one.</Text> : null}
      </ScrollView>}
      {offline && <View style={styles.offlineBanner}><Text style={styles.offlineBannerText}>OFFLINE · SHOWING CACHED FARM AND DEVICE DATA</Text></View>}
      {menuOpen && <Pressable style={styles.menuBackdrop} onPress={() => setMenuOpen(false)} accessibilityLabel="Close menu" />}
      <View style={[styles.dashboardHeader, dashboardView === "list" && styles.listHeader]}>
        {dashboardView === "list" ? <Text style={styles.listTitle} numberOfLines={1}>{currentFarm?.name || "Choose a farm"}</Text> : <View />}
        <View style={styles.headerActions}>
          <Pressable style={[styles.addButton, offline && styles.disabledButton]} onPress={openProvisioningDialog} disabled={busy || offline} accessibilityRole="button" accessibilityLabel="Add device"><Text style={styles.addButtonText}>+ ADD</Text></Pressable>
          <Pressable style={styles.menuButton} onPress={() => setMenuOpen((open) => !open)} accessibilityRole="button" accessibilityLabel="Open menu" accessibilityState={{ expanded: menuOpen }}><Text style={styles.menuButtonText}>MENU ⌄</Text></Pressable>
        </View>
        {menuOpen && <View style={styles.dropdownMenu}>
          <Pressable style={styles.menuItem} onPress={() => { setDashboardView(dashboardView === "map" ? "list" : "map"); setMenuOpen(false); }} accessibilityRole="menuitem"><Text style={styles.menuItemText}>{dashboardView === "map" ? "List view" : "Map view"}</Text></Pressable>
          <Pressable style={styles.menuItem} onPress={() => { setAlarmsOpen(true); setMenuOpen(false); }} accessibilityRole="menuitem"><Text style={styles.menuItemText}>Alarms{geofenceAlarms.filter((alarm) => !alarm.acknowledged).length ? ` · ${geofenceAlarms.filter((alarm) => !alarm.acknowledged).length}` : ""}</Text></Pressable>
          <Pressable style={styles.menuItem} onPress={openSettings} accessibilityRole="menuitem"><Text style={styles.menuItemText}>Settings · Farms</Text></Pressable>
          <View style={styles.menuDivider} />
          <Pressable style={styles.menuItem} onPress={() => void signOut().catch((caught) => setError(messageOf(caught)))} accessibilityRole="menuitem"><Text style={styles.menuItemText}>Sign out</Text></Pressable>
        </View>}
      </View>
    </View>}
    <Modal visible={alarmsOpen && Boolean(user)} animationType="slide" onRequestClose={() => setAlarmsOpen(false)}><SafeAreaView style={styles.dialogPage}><View style={styles.dialogHeader}><View><Text style={styles.stepLabel}>FARM SAFETY</Text><Text style={styles.dialogTitle}>Alarms</Text></View><Pressable onPress={() => setAlarmsOpen(false)}><Text style={styles.close}>CLOSE</Text></Pressable></View><ScrollView contentContainerStyle={styles.dialogContent}>{geofenceAlarms.length ? geofenceAlarms.map((alarm) => <View key={alarm.$id} style={styles.memberRow}><View style={styles.memberInfo}><Text style={styles.deviceNameDark}>{visibleDevices.find((device) => device.$id === alarm.deviceId)?.name || alarm.deviceId} · {alarm.event.toUpperCase()}</Text><Text style={styles.muted}>{geofenceAreas.find((area) => area.$id === alarm.areaId)?.name || "Area"} · {alarm.acknowledged ? "ACKNOWLEDGED" : "ACTIVE"}</Text><Text style={styles.muted}>{new Date(alarm.raisedAt).toLocaleString()}</Text></View>{!alarm.acknowledged && <Pressable onPress={() => void tables.updateRow<GeofenceAlarm>({ databaseId: config.databaseId, tableId: config.geofenceAlarmTableId, rowId: alarm.$id, data: { acknowledged: true, acknowledgedAt: new Date().toISOString() } }).then((updated) => setGeofenceAlarms((alarms) => alarms.map((item) => item.$id === updated.$id ? updated : item))).catch((caught) => setError(messageOf(caught)))}><Text style={styles.removeMemberText}>ACK</Text></Pressable>}</View>) : <Text style={styles.empty}>No active alarms.</Text>}</ScrollView></SafeAreaView></Modal>
    <Modal visible={settingsOpen && Boolean(user)} animationType="slide" onRequestClose={() => locationPickerOpen ? setLocationPickerOpen(false) : farmManagerOpen ? setFarmManagerOpen(false) : setSettingsOpen(false)}>
      {areaPickerOpen ? <AreaMapEditor draft={areaDraft} country={currentFarm?.country || ""} onCancel={() => setAreaPickerOpen(false)} onSave={(draft) => { setAreaDraft(draft); setAreaPickerOpen(false); }} /> : locationPickerOpen ? <FarmLocationPicker location={farmDraft.location} country={farmDraft.country} onCancel={() => setLocationPickerOpen(false)} onSelect={(location) => { setFarmDraft({ ...farmDraft, location }); setLocationPickerOpen(false); }} /> : <SafeAreaView style={styles.dialogPage}>
        <View style={styles.dialogHeader}><View><Text style={styles.stepLabel}>SETTINGS</Text><Text style={styles.dialogTitle}>{farmManagerOpen ? "Farm management" : "Farm settings"}</Text></View><Pressable onPress={() => farmManagerOpen ? setFarmManagerOpen(false) : setSettingsOpen(false)}><Text style={styles.close}>{farmManagerOpen ? "BACK" : "CLOSE"}</Text></Pressable></View>
        <ScrollView contentContainerStyle={styles.dialogContent} keyboardShouldPersistTaps="handled">
          {farmManagerOpen ? <>
            <Text style={styles.dialogHelp}>Switch farms or create and edit farms here. The selected farm opens automatically next time.</Text>
            {offline && <Text style={styles.dialogHelp}>Offline: cached farms can be viewed and switched. Connect to edit or create a farm.</Text>}
            {farms.map((farm) => <Pressable key={farm.$id} style={[styles.farmRow, farm.$id === currentFarmId && styles.farmRowSelected]} onPress={() => void selectFarm(farm).then(() => setFarmManagerOpen(false))} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: farm.$id === currentFarmId }}><Text style={styles.deviceNameDark}>{farm.name}</Text><Text style={styles.muted}>{farm.$id === currentFarmId ? "CURRENT FARM" : "TAP TO SWITCH"}</Text></Pressable>)}
            {!farms.length && <Text style={styles.muted}>No farms yet. Create your first farm.</Text>}
            <View style={styles.settingsActions}>
              <Pressable style={[styles.primary, styles.settingsAction, offline && styles.disabledButton]} onPress={() => { setFarmDraft(emptyFarmDetails); setFarmFormMode("create"); setError(""); }} disabled={busy || offline}><Text style={styles.primaryText}>CREATE FARM</Text></Pressable>
              {currentFarm?.ownerId === user?.$id && <Pressable style={[styles.outlineButton, styles.settingsAction, offline && styles.disabledButton]} onPress={() => { setFarmDraft(detailsFromFarm(currentFarm)); setFarmFormMode("edit"); setError(""); }} disabled={busy || offline}><Text style={styles.outlineButtonText}>EDIT FARM</Text></Pressable>}
            </View>
            {farmFormMode && <>
              <Text style={styles.fieldLabel}>{farmFormMode === "create" ? "CREATE FARM" : `EDIT ${currentFarm?.name || "FARM"}`}</Text>
              <FarmFields value={farmDraft} onChange={setFarmDraft} onPickLocation={() => setLocationPickerOpen(true)} />
              <View style={styles.settingsActions}>
                <Pressable style={[styles.primary, styles.settingsAction]} onPress={() => void (farmFormMode === "create" ? createFarm() : saveFarm())} disabled={busy}><Text style={styles.primaryText}>{farmFormMode === "create" ? "CREATE FARM & TEAM" : "SAVE FARM"}</Text></Pressable>
                <Pressable style={[styles.outlineButton, styles.settingsAction]} onPress={() => { setFarmFormMode(null); setError(""); }} disabled={busy}><Text style={styles.outlineButtonText}>CANCEL</Text></Pressable>
              </View>
            </>}
          </> : <>
            <Text style={styles.dialogHelp}>Manage the team, geofence areas, and rules for the current farm.</Text>
            {currentFarm ? <><View style={styles.currentFarmCard}><Text style={styles.deviceNameDark}>{currentFarm.name}</Text><Text style={styles.muted}>{currentFarm.location}, {currentFarm.country} · Channel {currentFarm.halowChannel}</Text><Pressable style={styles.outlineButton} onPress={() => { setFarmFormMode(null); setFarmManagerOpen(true); }}><Text style={styles.outlineButtonText}>MANAGE FARMS</Text></Pressable></View><View style={styles.settingsTabs}>{(["team", "areas", "rules"] as SettingsTab[]).map((tab) => <Pressable key={tab} style={[styles.settingsTab, settingsTab === tab && styles.settingsTabActive]} onPress={() => { setTeamError(""); setSettingsTab(tab); }} accessibilityRole="tab" accessibilityState={{ selected: settingsTab === tab }}><Text style={[styles.settingsTabText, settingsTab === tab && styles.settingsTabTextActive]}>{tab.toUpperCase()}</Text></Pressable>)}</View></> : <Pressable style={styles.primary} onPress={() => setFarmManagerOpen(true)}><Text style={styles.primaryText}>MANAGE FARMS</Text></Pressable>}
          </>}
          {!farmManagerOpen && <>
          {currentFarm && settingsTab === "team" && <>
            <Text style={styles.sectionLabel}>FARM TEAM</Text>
            <Text style={styles.dialogHelp}>Team members can view this farm, its devices, and their readings.</Text>
            {offline ? <Text style={styles.muted}>Connect to view and manage team members.</Text> : membersLoading ? <ActivityIndicator color="#0a8c87" /> : members.length ? members.map((member) => {
              const isCurrentOwner = currentFarm.ownerId === user?.$id && member.roles.includes("owner");
              const displayName = isCurrentOwner ? user.name?.trim() || member.userName?.trim() : member.userName?.trim();
              const displayEmail = member.userEmail || (isCurrentOwner ? user.email : "");
              const status = member.confirm ? (member.roles.includes("owner") ? "Owner" : "Member") : "Invitation pending";
              return <View key={member.$id} style={styles.memberRow}><View style={styles.memberInfo}><Text style={styles.deviceNameDark}>{displayEmail || displayName || "Member details hidden by Appwrite"}</Text><Text style={styles.muted}>{displayEmail && displayName ? `${displayName} · ${status}` : status}</Text></View>{currentFarm.ownerId === user?.$id && !member.roles.includes("owner") && member.userId !== user.$id && <Pressable onPress={() => requestRemoveMember(member)} disabled={busy} accessibilityRole="button" accessibilityLabel={`Remove ${displayEmail || displayName || "member"}`}><Text style={styles.removeMemberText}>REMOVE</Text></Pressable>}</View>;
            }) : <Text style={styles.muted}>No team members found.</Text>}
            {!offline && <>
              <Text style={styles.fieldLabel}>YOUR NAME</Text>
              <TextInput style={styles.inputLight} value={profileName} onChangeText={setProfileName} placeholder="Your name in the team" autoCapitalize="words" autoComplete="name" />
              <Pressable style={[styles.outlineButton, !profileName.trim() && styles.disabledButton]} onPress={() => void saveProfileName()} disabled={busy || !profileName.trim() || profileName.trim() === user?.name?.trim()}><Text style={styles.outlineButtonText}>SAVE NAME</Text></Pressable>
            </>}
            {currentFarm.ownerId === user?.$id && !offline && <>
              <Text style={styles.sectionLabel}>INVITE MEMBER</Text>
              <Text style={styles.fieldLabel}>NAME (OPTIONAL)</Text>
              <TextInput style={styles.inputLight} value={inviteName} onChangeText={setInviteName} placeholder="Member's name" placeholderTextColor="#59716f" autoCapitalize="words" autoComplete="name" />
              <Text style={styles.fieldLabel}>EMAIL (REQUIRED)</Text>
              <TextInput style={styles.inputLight} value={inviteEmail} onChangeText={setInviteEmail} placeholder="name@example.com" placeholderTextColor="#59716f" autoCapitalize="none" keyboardType="email-address" autoComplete="email" />
              <Pressable style={styles.primary} onPress={() => void inviteMember()} disabled={busy || !inviteEmail.trim()}><Text style={styles.primaryText}>SEND INVITATION</Text></Pressable>
            </>}
            {teamError ? <Text style={styles.dialogError}>{teamError}</Text> : null}
          </>}
          {currentFarm && settingsTab === "areas" && <>
            <Text style={styles.sectionLabel}>GEOFENCE AREAS</Text>
            <Text style={styles.dialogHelp}>Areas are stored as a circle, oval, rectangle, or polygon. Rules are managed separately below.</Text>
            {geofenceAreas.map((area) => <View key={area.$id} style={styles.memberRow}><View style={[styles.areaColorSwatch, { backgroundColor: savedAreaColor(area) }]} /><View style={styles.memberInfo}><Text style={styles.deviceNameDark}>{area.name}</Text><Text style={styles.muted}>{area.shape.toUpperCase()}</Text></View>{currentFarm.ownerId === user?.$id && <Pressable onPress={() => { setTeamError(""); let geometry: { center?: { latitude: number; longitude: number }; radiusMeters?: number; radiusXMeters?: number; radiusYMeters?: number; widthMeters?: number; heightMeters?: number; vertices?: { latitude: number; longitude: number }[]; color?: string } = {}; try { geometry = JSON.parse(area.geometry); } catch {} setAreaEditingId(area.$id); setAreaDraft({ name: area.name, shape: area.shape, location: geometry.center ? `${geometry.center.latitude}, ${geometry.center.longitude}` : "", primary: String(geometry.radiusMeters ?? geometry.radiusXMeters ?? geometry.widthMeters ?? 100), secondary: String(geometry.radiusYMeters ?? geometry.heightMeters ?? 100), vertices: (geometry.vertices || []).map((point) => `${point.latitude},${point.longitude}`).join(";"), color: areaColor(geometry.color) }); }}><Text style={styles.removeMemberText}>EDIT</Text></Pressable>}</View>)}
            {currentFarm.ownerId === user?.$id && <>
              <Pressable style={styles.outlineButton} onPress={() => { setTeamError(""); setAreaEditingId("new"); setAreaDraft({ ...emptyAreaDraft, location: currentFarm.location }); }}><Text style={styles.outlineButtonText}>ADD AREA</Text></Pressable>
              {areaEditingId && <View style={styles.geofenceForm}>
                <TextInput style={styles.inputLight} value={areaDraft.name} onChangeText={(name) => setAreaDraft({ ...areaDraft, name })} placeholder="Area name" />
                <View style={styles.settingsActions}>{(["circle", "oval", "rectangle", "polygon"] as GeofenceShape[]).map((shape) => <Pressable key={shape} style={[styles.smallOption, areaDraft.shape === shape && styles.farmRowSelected]} onPress={() => setAreaDraft({ ...areaDraft, shape })}><Text style={styles.deviceNameDark}>{shape.toUpperCase()}</Text></Pressable>)}</View>
                <AreaColorPicker color={areaDraft.color} onChange={(color) => setAreaDraft((draft) => ({ ...draft, color }))} />
                <Pressable style={styles.farmRow} onPress={() => setAreaPickerOpen(true)}><Text style={styles.deviceNameDark}>{areaDraft.shape === "polygon" ? "EDIT POINTS ON MAP" : "CENTER ON MAP"}</Text><Text style={styles.muted}>{areaDraft.shape === "polygon" ? `${areaDraft.vertices.split(";").filter(Boolean).length} points` : areaDraft.location || "Choose center"}</Text></Pressable>
                {areaDraft.shape !== "polygon" && <View style={styles.settingsActions}><TextInput style={[styles.inputLight, styles.dimensionInput]} value={areaDraft.primary} onChangeText={(primary) => setAreaDraft({ ...areaDraft, primary })} placeholder={areaDraft.shape === "circle" ? "Radius m" : "Width/radius m"} keyboardType="decimal-pad" />{areaDraft.shape !== "circle" && <TextInput style={[styles.inputLight, styles.dimensionInput]} value={areaDraft.secondary} onChangeText={(secondary) => setAreaDraft({ ...areaDraft, secondary })} placeholder="Height/radius m" keyboardType="decimal-pad" />}</View>}
                <Pressable style={styles.primary} onPress={() => void (async () => { setTeamError(""); try { const geometry = areaGeometry(areaDraft); const data = { farmId: currentFarm.$id, name: areaDraft.name.trim(), shape: areaDraft.shape, geometry }; const permissions = [Permission.read(Role.team(currentFarm.teamId)), Permission.update(Role.team(currentFarm.teamId, "owner")), Permission.delete(Role.team(currentFarm.teamId, "owner"))]; const area = areaEditingId === "new" ? await tables.createRow<GeofenceArea>({ databaseId: config.databaseId, tableId: config.geofenceAreaTableId, rowId: ID.unique(), data, permissions }) : await tables.updateRow<GeofenceArea>({ databaseId: config.databaseId, tableId: config.geofenceAreaTableId, rowId: areaEditingId, data }); setGeofenceAreas((items) => areaEditingId === "new" ? [...items, area] : items.map((item) => item.$id === area.$id ? area : item)); setAreaEditingId(null); } catch (caught) { setTeamError(messageOf(caught)); } })()}><Text style={styles.primaryText}>SAVE AREA</Text></Pressable>
                {teamError ? <Text style={styles.dialogError}>{teamError}</Text> : null}
              </View>}
            </>}
          </>}
          {currentFarm && settingsTab === "rules" && <>
            <Text style={styles.sectionLabel}>GEOFENCE RULES</Text>
            {geofenceRules.map((rule) => <View key={rule.$id} style={styles.memberRow}><View style={styles.memberInfo}><Text style={styles.deviceNameDark}>{rule.name || "Unnamed rule"}</Text><Text style={styles.muted}>{geofenceAreas.find((area) => area.$id === rule.areaId)?.name || "Deleted area"} · {rule.enterAlert ? "ENTER" : ""}{rule.enterAlert && rule.exitAlert ? " + " : ""}{rule.exitAlert ? "LEAVE" : ""} · {rule.deviceIds?.length || "All"} devices</Text></View></View>)}
            {currentFarm.ownerId === user?.$id && <View style={styles.geofenceForm}><Text style={styles.dialogHelp}>Name the rule, select its area, and choose the devices it applies to. Leave device selection empty for every farm device.</Text><Text style={styles.fieldLabel}>RULE NAME</Text><TextInput style={styles.inputLight} value={ruleName} onChangeText={setRuleName} placeholder="For example: Cattle leave north pasture" maxLength={128} /><Text style={styles.fieldLabel}>AREA</Text>{geofenceAreas.length ? geofenceAreas.map((area) => <Pressable key={area.$id} style={[styles.farmRow, ruleAreaId === area.$id && styles.farmRowSelected]} onPress={() => setRuleAreaId(area.$id)} accessibilityRole="radio" accessibilityState={{ selected: ruleAreaId === area.$id }}><Text style={styles.deviceNameDark}>{area.name}</Text><Text style={styles.muted}>{area.shape.toUpperCase()}{ruleAreaId === area.$id ? " · SELECTED" : ""}</Text></Pressable>) : <Text style={styles.muted}>Create an area before adding a rule.</Text>}<Text style={styles.fieldLabel}>DEVICES</Text>{visibleDevices.map((device) => <Pressable key={device.$id} style={[styles.farmRow, ruleDeviceIds.includes(device.$id) && styles.farmRowSelected]} onPress={() => setRuleDeviceIds((ids) => ids.includes(device.$id) ? ids.filter((id) => id !== device.$id) : [...ids, device.$id])}><Text style={styles.deviceNameDark}>{device.name}</Text></Pressable>)}<View style={styles.settingsActions}><Pressable style={[styles.smallOption, ruleEnter && styles.farmRowSelected]} onPress={() => setRuleEnter((value) => !value)}><Text style={styles.deviceNameDark}>ENTER</Text></Pressable><Pressable style={[styles.smallOption, ruleExit && styles.farmRowSelected]} onPress={() => setRuleExit((value) => !value)}><Text style={styles.deviceNameDark}>LEAVE</Text></Pressable></View><Pressable style={[styles.primary, (!ruleName.trim() || !ruleAreaId || (!ruleEnter && !ruleExit)) && styles.disabledButton]} onPress={() => void (async () => { if (!ruleName.trim() || !ruleAreaId || (!ruleEnter && !ruleExit)) { setTeamError("Enter a rule name, choose an area, and select at least one alarm condition."); return; } setTeamError(""); try { const rule = await tables.createRow<GeofenceRule>({ databaseId: config.databaseId, tableId: config.geofenceRuleTableId, rowId: ID.unique(), data: { farmId: currentFarm.$id, name: ruleName.trim(), areaId: ruleAreaId, deviceIds: ruleDeviceIds, enterAlert: ruleEnter, exitAlert: ruleExit }, permissions: [Permission.read(Role.team(currentFarm.teamId)), Permission.update(Role.team(currentFarm.teamId, "owner")), Permission.delete(Role.team(currentFarm.teamId, "owner"))] }); setGeofenceRules((rules) => [...rules, rule]); setRuleName(""); setRuleAreaId(""); setRuleDeviceIds([]); } catch (caught) { setTeamError(messageOf(caught)); } })()} disabled={!ruleName.trim() || !ruleAreaId || (!ruleEnter && !ruleExit)}><Text style={styles.primaryText}>SAVE RULE</Text></Pressable>{teamError ? <Text style={styles.dialogError}>{teamError}</Text> : null}</View>}
          </>}
          </>}
          {error ? <Text style={styles.dialogError}>{error}</Text> : null}
        </ScrollView>
      </SafeAreaView>}
    </Modal>
    <Modal visible={provisioningDialogOpen} animationType="slide" onRequestClose={() => deviceLocationPickerOpen ? setDeviceLocationPickerOpen(false) : closeProvisioningDialog()}>
      {deviceLocationPickerOpen ? <FarmLocationPicker device location={deviceLocation || currentFarm?.location || ""} country={currentFarm?.country || ""} onCancel={() => setDeviceLocationPickerOpen(false)} onSelect={(location) => { setDeviceLocation(location); setDeviceLocationChoice("map"); setDeviceLocationPickerOpen(false); }} /> : <SafeAreaView style={styles.dialogPage}>
        <KeyboardAvoidingView style={styles.dialogScreen} behavior={Platform.OS === "ios" ? "padding" : undefined} accessibilityViewIsModal>
          <View style={styles.dialogHeader}><View><Text style={styles.stepLabel}>STEP {selectedBleDevice && isNrfDevice(selectedBleDevice) ? (provisioningStep === 4 ? 3 : provisioningStep) : provisioningStep} OF {selectedBleDevice && isNrfDevice(selectedBleDevice) ? 3 : 4}</Text><Text style={styles.dialogTitle}>{provisioningStep === 1 ? "Choose device" : provisioningStep === 2 ? "Device details" : provisioningStep === 3 ? "Upstream Wi-Fi" : "Confirm setup"}</Text></View><Pressable onPress={closeProvisioningDialog} disabled={busy}><Text style={styles.close}>CLOSE</Text></Pressable></View>
          <ScrollView contentContainerStyle={styles.dialogContent} keyboardShouldPersistTaps="handled">
            {provisioningStep === 1 && <>
              <Text style={styles.dialogHelp}>Put the ESP32 in provisioning mode or power on the nRF54, then scan for its Bluetooth name.</Text>
              <Pressable style={styles.primary} onPress={scanBleDevices} disabled={busy}><Text style={styles.primaryText}>{busy ? "SCANNING…" : "SCAN FOR DEVICES"}</Text></Pressable>
              {bleDevices.map((device) => <Pressable key={device.name} style={styles.bleDevice} onPress={() => selectBleDevice(device)} disabled={busy}><Text style={styles.deviceNameDark}>{device.name}</Text><Text style={styles.muted}>Serial {serialFromBleName(device.name)}</Text></Pressable>)}
            </>}
            {provisioningStep === 2 && selectedBleDevice && <>
              <View style={styles.selectedSummary}><Text style={styles.deviceNameDark}>{selectedBleDevice.name}</Text><Text style={styles.muted}>Serial {serialFromBleName(selectedBleDevice.name)}</Text></View>
              <Text style={styles.fieldLabel}>NAME</Text>
              <TextInput style={styles.inputLight} value={name} onChangeText={setName} placeholder="Device name" maxLength={128} />
              <Text style={styles.fieldHint}>Optional. The serial is used when no name is entered.</Text>
              <MapAppearancePicker icon={deviceIcon} color={deviceColor} onIconChange={setDeviceIcon} onColorChange={setDeviceColor} />
              <Text style={styles.fieldLabel}>LOCATION · OPTIONAL</Text>
              <Pressable style={[styles.farmRow, deviceLocationChoice === "none" && styles.farmRowSelected]} onPress={() => { setDeviceLocationChoice("none"); setDeviceLocation(""); }} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: deviceLocationChoice === "none" }}><Text style={styles.deviceNameDark}>None</Text></Pressable>
              <Pressable style={[styles.farmRow, deviceLocationChoice === "current" && styles.farmRowSelected]} onPress={() => void chooseCurrentDeviceLocation()} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: deviceLocationChoice === "current" }}><Text style={styles.deviceNameDark}>{busy ? "Finding current location…" : "Current location"}</Text>{deviceLocationChoice === "current" && <Text style={styles.muted}>{deviceLocation}</Text>}</Pressable>
              <Pressable style={[styles.farmRow, deviceLocationChoice === "map" && styles.farmRowSelected]} onPress={() => setDeviceLocationPickerOpen(true)} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: deviceLocationChoice === "map" }}><Text style={styles.deviceNameDark}>Choose on map</Text>{deviceLocationChoice === "map" && <Text style={styles.muted}>{deviceLocation}</Text>}</Pressable>
              {isNrfDevice(selectedBleDevice) && <Pressable style={[styles.farmRow, deviceLocationChoice === "gps" && styles.farmRowSelected]} onPress={() => { setDeviceLocationChoice("gps"); setDeviceLocation(""); }} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: deviceLocationChoice === "gps" }}><Text style={styles.deviceNameDark}>Device GPS</Text><Text style={styles.muted}>Use the GPS connected to this nRF54</Text></Pressable>}
              {!isNrfDevice(selectedBleDevice) && <><Text style={styles.fieldLabel}>PROOF OF POSSESSION (PoP)</Text><TextInput style={styles.inputLight} value={proofOfPossession} onChangeText={setProofOfPossession} placeholder="PoP shown on the device OLED" autoCapitalize="none" autoCorrect={false} /></>}
              <Pressable style={styles.primary} onPress={connectForProvisioning} disabled={busy || (!isNrfDevice(selectedBleDevice) && !proofOfPossession.trim())}><Text style={styles.primaryText}>{busy ? "CONNECTING…" : "CONNECT DEVICE"}</Text></Pressable>
            </>}
            {provisioningStep === 3 && selectedBleDevice && <>
              <Text style={styles.dialogHelp}>Will this ESP32 use regular Wi-Fi for its upstream connection? It will receive the farm mesh settings either way.</Text>
              <Pressable style={styles.farmRow} onPress={() => void chooseUpstreamWifi(true)} disabled={busy}><Text style={styles.deviceNameDark}>Yes, connect to Wi-Fi</Text></Pressable>
              <Pressable style={styles.farmRow} onPress={() => { void chooseUpstreamWifi(false); setProvisioningStep(4); }} disabled={busy}><Text style={styles.deviceNameDark}>No upstream Wi-Fi</Text></Pressable>
              {useUpstreamWifi && <>
                {upstreamNetworks.map((network) => <Pressable key={`${network.ssid}-${network.bssid || ""}`} style={[styles.farmRow, upstreamSsid === network.ssid && styles.farmRowSelected]} onPress={() => { setUpstreamSsid(network.ssid); setUpstreamPassword(""); }} disabled={busy}><Text style={styles.deviceNameDark}>{network.ssid}</Text><Text style={styles.muted}>{network.rssi} dBm</Text></Pressable>)}
                <Text style={styles.fieldLabel}>WI-FI SSID</Text>
                <TextInput style={styles.inputLight} value={upstreamSsid} onChangeText={setUpstreamSsid} placeholder="Network name" maxLength={32} />
                <Text style={styles.fieldLabel}>WI-FI PASSWORD</Text>
                <TextInput style={styles.inputLight} value={upstreamPassword} onChangeText={setUpstreamPassword} placeholder="Leave blank for an open network" secureTextEntry maxLength={63} />
                <Pressable style={styles.primary} onPress={() => setProvisioningStep(4)} disabled={busy || !upstreamSsid.trim()}><Text style={styles.primaryText}>CONTINUE</Text></Pressable>
              </>}
            </>}
            {provisioningStep === 4 && selectedBleDevice && <>
              <View style={styles.selectedSummary}><Text style={styles.deviceNameDark}>{currentFarm?.name}</Text><Text style={styles.muted}>{currentFarm?.location}, {currentFarm?.country} · Channel {currentFarm?.halowChannel}</Text><Text style={styles.muted}>Mesh ID: {currentFarm?.meshId}</Text><Text style={styles.muted}>Device location: {deviceLocationChoice === "none" ? "None" : deviceLocationChoice === "gps" ? "Device GPS" : deviceLocation}</Text></View>
              <Text style={styles.dialogHelp}>{isNrfDevice(selectedBleDevice) ? "HaLow upstream. " : useUpstreamWifi ? `Upstream Wi-Fi: ${upstreamSsid}. ` : "No upstream Wi-Fi. "}The app will send the mesh settings and MQTT credential to the device.</Text>
              <Pressable style={styles.primary} onPress={provisionDevice} disabled={busy || !bleConnected || !currentFarm}><Text style={styles.primaryText}>{busy ? "PROVISIONING…" : "PROVISION DEVICE"}</Text></Pressable>
            </>}
            {provisioningStep > 1 && <Pressable style={styles.backButton} onPress={previousProvisioningStep} disabled={busy}><Text style={styles.secondaryText}>BACK</Text></Pressable>}
            {provisioningStatus ? <Text style={styles.dialogStatus}>{provisioningStatus}</Text> : null}
            {error ? <Text style={styles.dialogError}>{error}</Text> : null}
          </ScrollView>
        </KeyboardAvoidingView>
      </SafeAreaView>}
    </Modal>
    <Modal visible={Boolean(detailDevice)} animationType="slide" onRequestClose={() => setDetailDevice(null)}>
      <SafeAreaView style={styles.detailPage}>
        {detailDevice ? <>
          <View style={styles.detailHeader}><Pressable onPress={() => setDetailDevice(null)}><Text style={styles.detailBack}>‹ DEVICES</Text></Pressable><Text style={styles.detailSerial}>{detailDevice.serial}</Text></View>
          <ScrollView contentContainerStyle={styles.detailContent}>
            <View style={styles.detailTitleRow}><View><Text style={styles.detailEyebrow}>DEVICE</Text><Text style={styles.detailTitle}>{detailDevice.name}</Text></View><View style={styles.detailStatus}><View style={[styles.statusDot, detailStatus === "Online" ? styles.statusOnline : styles.statusOffline]} /><Text style={styles.detailStatusText}>{detailStatus.toUpperCase()}</Text></View></View>
            <MapAppearancePicker icon={detailIcon} color={detailColor} onIconChange={setDetailIcon} onColorChange={setDetailColor} />
            <Pressable style={[styles.primary, (offline || busy || (detailIcon === detailDevice.metadata?.icon && detailColor === colorForDevice(detailDevice))) && styles.disabledButton]} onPress={() => void saveDeviceAppearance()} disabled={offline || busy || (detailIcon === detailDevice.metadata?.icon && detailColor === colorForDevice(detailDevice))}><Text style={styles.primaryText}>SAVE MAP APPEARANCE</Text></Pressable>
            {error ? <Text style={styles.dialogError}>{error}</Text> : null}
            <View style={styles.detailLatest}><Text style={styles.detailMetricLabel}>BATTERY VOLTAGE</Text><Text style={styles.detailMetricValue}>{detailLatest ? `${detailLatest.value.toFixed(2)} V` : "—"}</Text><Text style={styles.detailMetricTime}>{detailLatest ? `Updated ${relativeTime(detailLatest.row.receivedAt)}` : "No readings received"}</Text></View>
            <Text style={styles.rangeTitle}>HISTORY RANGE</Text>
            <View style={styles.rangeSelector}>{historyRanges.map((range) => <Pressable key={range.key} style={[styles.rangeButton, historyRange === range.key && styles.rangeButtonActive]} onPress={() => setHistoryRange(range.key)} disabled={historyLoading}><Text style={[styles.rangeButtonText, historyRange === range.key && styles.rangeButtonTextActive]}>{range.label}</Text></Pressable>)}</View>
            <View style={styles.chartCard}><View style={styles.chartCardHeader}><View><Text style={styles.chartTitle}>Battery voltage</Text><Text style={styles.chartSubtitle}>Device battery ADC · {historyPoints.length} readings{offline ? " · cached" : ""}</Text></View>{historyLoading ? <ActivityIndicator color="#0a8c87" /> : null}</View><VoltageChart points={historyPoints} duration={activeHistoryDuration} /><View style={styles.chartAxis}><Text style={styles.chartAxisText}>{activeHistoryRange.label} AGO</Text><Text style={styles.chartAxisText}>NOW</Text></View>{historyError ? <Text style={styles.historyError}>{historyError}</Text> : null}</View>
            {historyStats ? <View style={styles.statsRow}><View style={styles.stat}><Text style={styles.statLabel}>MIN</Text><Text style={styles.statValue}>{historyStats.min.toFixed(2)} V</Text></View><View style={styles.stat}><Text style={styles.statLabel}>AVERAGE</Text><Text style={styles.statValue}>{historyStats.average.toFixed(2)} V</Text></View><View style={styles.stat}><Text style={styles.statLabel}>MAX</Text><Text style={styles.statValue}>{historyStats.max.toFixed(2)} V</Text></View></View> : null}
            <Text style={styles.sensorNote}>Battery voltage is measured by the device ADC; no reading appears when a battery is disconnected.</Text>
            <TopologyCard device={detailDevice} links={detailTopology} />
            {detailDevice.metadata?.firmwareTarget === "heltec-hc33" || detailFirmwareVersion ? <View style={styles.otaZone}><Text style={styles.otaTitle}>FIRMWARE UPDATE</Text><Text style={styles.otaDescription}>Running {detailFirmwareVersion || "version unknown"}. Install the latest HT-HC33 OTA image from this deployment&apos;s source repository.</Text>{detailOtaUpdate ? <Text style={styles.otaDescription}>Latest update: {detailOtaUpdate.status.toUpperCase()}{detailOtaUpdate.detail ? ` · ${detailOtaUpdate.detail}` : ""}{detailOtaUpdate.reportedAt ? ` · ${relativeTime(detailOtaUpdate.reportedAt)}` : ""}</Text> : null}<Pressable style={[styles.otaButton, (offline || busy || detailStatus !== "Online" || !otaImageUrl || detailOtaPending) && styles.disabledButton]} onPress={() => requestFirmwareUpdate(detailDevice)} disabled={offline || busy || detailStatus !== "Online" || !otaImageUrl || detailOtaPending}><Text style={styles.otaButtonText}>{busy ? "PLEASE WAIT…" : detailOtaPending ? "UPDATE PENDING" : "UPDATE HT-HC33"}</Text></Pressable></View> : null}
            <View style={styles.dangerZone}><Text style={styles.dangerTitle}>DEVICE ACCESS</Text><Text style={styles.dangerDescription}>Deleting this device revokes its MQTT credential. Historical telemetry is retained.</Text><Pressable style={[styles.deleteButton, offline && styles.disabledButton]} onPress={() => requestDeleteDevice(detailDevice)} disabled={busy || offline}><Text style={styles.deleteButtonText}>{busy ? "DELETING…" : "DELETE DEVICE"}</Text></Pressable></View>
          </ScrollView>
        </> : null}
      </SafeAreaView>
    </Modal>
    {error ? <Text style={styles.error}>{error}</Text> : null}
  </KeyboardAvoidingView></SafeAreaView></SafeAreaProvider>;
}

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: "#092e35" }, loader: { flex: 1 }, screen: { flex: 1, paddingHorizontal: 22, paddingTop: 18 }, dashboardScreen: { paddingHorizontal: 0, paddingTop: 0 }, dashboard: { flex: 1 },
  header: { flexDirection: "row", justifyContent: "space-between", alignItems: "center", paddingBottom: 20 }, headerActions: { flexDirection: "row", alignItems: "center", gap: 14 }, dashboardHeader: { position: "absolute", top: 0, left: 0, right: 0, minHeight: 68, paddingHorizontal: 22, paddingTop: 18, flexDirection: "row", justifyContent: "space-between", alignItems: "center", zIndex: 2 }, listHeader: { backgroundColor: "#092e35" }, listTitle: { color: "white", fontSize: 24, fontWeight: "800", flexShrink: 1, marginRight: 8 }, menuBackdrop: { ...StyleSheet.absoluteFill, zIndex: 1 }, menuButton: { minHeight: 36, minWidth: 70, paddingHorizontal: 10, borderRadius: 10, alignItems: "center", justifyContent: "center", backgroundColor: "#092e35e8" }, menuButtonText: { color: "white", fontSize: 10, fontWeight: "900", letterSpacing: .7 }, dropdownMenu: { position: "absolute", top: 62, right: 22, minWidth: 156, paddingVertical: 5, borderRadius: 12, backgroundColor: "#092e35", elevation: 8, shadowColor: "#000", shadowOpacity: .25, shadowRadius: 12, shadowOffset: { width: 0, height: 5 } }, menuItem: { minHeight: 44, paddingHorizontal: 16, justifyContent: "center" }, menuItemText: { color: "white", fontSize: 13, fontWeight: "700" }, menuItemActive: { color: "#69cfc7" }, menuDivider: { height: 1, marginHorizontal: 10, backgroundColor: "#36565b" }, addButton: { minHeight: 36, paddingHorizontal: 13, borderRadius: 10, alignItems: "center", justifyContent: "center", backgroundColor: "#0a8c87" }, addButtonText: { color: "white", fontSize: 10, fontWeight: "900", letterSpacing: .7 }, eyebrow: { color: "#69cfc7", fontSize: 10, fontWeight: "900", letterSpacing: 1.4 }, title: { color: "#fff", fontSize: 28, fontWeight: "800", marginTop: 3 },
  offlineBanner: { position: "absolute", top: 70, left: 16, right: 16, zIndex: 1, borderRadius: 9, padding: 10, backgroundColor: "#092e35e8" }, offlineBannerText: { color: "white", fontSize: 10, fontWeight: "800", textAlign: "center", letterSpacing: .3 }, disabledButton: { opacity: .45 },
  auth: { flex: 1, justifyContent: "center", gap: 8, paddingBottom: 40 }, hero: { color: "white", fontSize: 45, lineHeight: 51, fontWeight: "900", letterSpacing: -1.8, marginBottom: 24 }, accent: { color: "#ff8264" }, authLabel: { color: "#90aaa7", fontSize: 9, fontWeight: "900", letterSpacing: 1, marginTop: 5 },
  input: { height: 58, borderWidth: 1, borderColor: "#36565b", borderRadius: 14, paddingHorizontal: 17, color: "white", fontSize: 16 }, primary: { minHeight: 54, borderRadius: 14, alignItems: "center", justifyContent: "center", backgroundColor: "#0a8c87", marginTop: 5 }, primaryText: { color: "white", fontSize: 10, fontWeight: "900", letterSpacing: .8 }, secondary: { minHeight: 52, alignItems: "center", justifyContent: "center" }, secondaryText: { color: "#69cfc7", fontSize: 11, fontWeight: "900", letterSpacing: 1 },
  listScroll: { flex: 1 }, listContent: { paddingHorizontal: 22, paddingTop: 94, paddingBottom: 30, gap: 12 }, inputLight: { height: 54, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 12, paddingHorizontal: 15, color: "#0a3037", backgroundColor: "white" }, muted: { color: "#59716f", fontSize: 11 },
  mapCard: { flex: 1, overflow: "hidden", backgroundColor: "#dce8e5" }, map: { ...StyleSheet.absoluteFill }, mapPrompt: { position: "absolute", left: 12, right: 12, bottom: 28, padding: 14, borderRadius: 14, backgroundColor: "#092e35f2" }, mapPromptTitle: { color: "white", fontSize: 14, fontWeight: "900" }, mapPromptText: { color: "#b8cdca", fontSize: 11, lineHeight: 16, marginTop: 3 }, mapPromptActions: { flexDirection: "row", gap: 8, marginTop: 10 }, mapDownloadButton: { minHeight: 38, justifyContent: "center", paddingHorizontal: 13, borderRadius: 9, backgroundColor: "#0a8c87" }, mapDownloadText: { color: "white", fontSize: 9, fontWeight: "900", letterSpacing: .7 }, mapLaterButton: { minHeight: 38, justifyContent: "center", paddingHorizontal: 13 }, mapLaterText: { color: "#90aaa7", fontSize: 9, fontWeight: "900", letterSpacing: .7 }, mapNotice: { position: "absolute", left: 12, right: 90, top: 68, borderRadius: 9, padding: 9, backgroundColor: "#092e35e8" }, mapError: { backgroundColor: "#8f3422e8" }, mapNoticeText: { color: "white", fontSize: 10, fontWeight: "700" }, mapAttribution: { position: "absolute", left: 10, right: 10, bottom: 5, color: "#173e43", fontSize: 9, textShadowColor: "white", textShadowRadius: 4 },
  farmRow: { padding: 16, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white", gap: 4 }, currentFarmCard: { padding: 16, borderRadius: 14, backgroundColor: "#e9f7f5", gap: 8 }, settingsTabs: { flexDirection: "row", padding: 4, borderRadius: 12, backgroundColor: "#dce8e5" }, settingsTab: { flex: 1, minHeight: 38, borderRadius: 9, alignItems: "center", justifyContent: "center" }, settingsTabActive: { backgroundColor: "#0a8c87" }, settingsTabText: { color: "#59716f", fontSize: 9, fontWeight: "900", letterSpacing: .5 }, settingsTabTextActive: { color: "white" }, farmRowSelected: { borderColor: "#0a8c87", borderWidth: 2, backgroundColor: "#e9f7f5" },
  geofenceForm: { gap: 10, padding: 12, borderRadius: 12, backgroundColor: "#e9f2f0" }, smallOption: { flex: 1, minHeight: 42, paddingHorizontal: 8, borderRadius: 10, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white", alignItems: "center", justifyContent: "center" }, dimensionInput: { flex: 1 },
  settingsActions: { flexDirection: "row", gap: 10 }, settingsAction: { flex: 1 }, outlineButton: { minHeight: 54, borderRadius: 14, borderWidth: 1, borderColor: "#0a8c87", alignItems: "center", justifyContent: "center", marginTop: 5 }, outlineButtonText: { color: "#0a8c87", fontSize: 10, fontWeight: "900", letterSpacing: .8 }, memberRow: { padding: 14, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white", flexDirection: "row", alignItems: "center", gap: 10 }, memberInfo: { flex: 1, gap: 4 }, removeMemberText: { color: "#b9472f", fontSize: 10, fontWeight: "900" },
  iconChoices: { gap: 8, paddingVertical: 4 }, iconChoice: { minHeight: 58, minWidth: 72, gap: 3, paddingHorizontal: 12, paddingVertical: 5, borderRadius: 10, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white", alignItems: "center", justifyContent: "center" }, areaColorSwatch: { width: 24, height: 24, borderRadius: 12, borderWidth: 1, borderColor: "#0a303744" },
  selectField: { minHeight: 54, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 12, paddingHorizontal: 15, backgroundColor: "white", flexDirection: "row", justifyContent: "space-between", alignItems: "center", gap: 8 }, selectText: { color: "#0a3037", fontSize: 14 }, optionList: { borderWidth: 1, borderColor: "#cedbdc", borderRadius: 12, backgroundColor: "white", overflow: "hidden" }, optionRow: { minHeight: 46, paddingHorizontal: 15, justifyContent: "center", borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: "#dce6e5" },
  passphraseField: { height: 54, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 12, backgroundColor: "white", flexDirection: "row", alignItems: "center" }, passphraseInput: { flex: 1, height: 52, paddingLeft: 15, color: "#0a3037" }, visibilityButton: { width: 54, height: 52, alignItems: "center", justifyContent: "center" },
  locationMap: { flex: 1, overflow: "hidden", backgroundColor: "#dce8e5" }, mapCrosshair: { position: "absolute", left: "50%", top: "50%", marginLeft: -18, marginTop: -26, width: 36, height: 52, alignItems: "center", justifyContent: "center" }, mapCrosshairText: { color: "#0a8c87", fontSize: 42, fontWeight: "900", textShadowColor: "white", textShadowRadius: 4 }, locationFooter: { paddingHorizontal: 22, paddingVertical: 12, gap: 5, backgroundColor: "#f7faf9" },
  areaMapControls: { position: "absolute", right: 16, bottom: 20, gap: 8, alignItems: "flex-end" }, areaDimension: { gap: 4, alignItems: "flex-end" }, areaControlButtons: { flexDirection: "row", gap: 6 }, areaControl: { width: 42, height: 42, borderRadius: 21, alignItems: "center", justifyContent: "center", backgroundColor: "#092e35e8" }, areaControlText: { color: "white", fontSize: 28, fontWeight: "600" }, areaControlLabel: { color: "white", fontSize: 10, fontWeight: "800", paddingHorizontal: 10, paddingVertical: 7, borderRadius: 8, backgroundColor: "#092e35e8" },
  bleDevice: { padding: 13, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white" }, deviceNameDark: { color: "#0a3037", fontSize: 14, fontWeight: "800" },
  wifiHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", marginTop: 4 }, rescan: { color: "#0a8c87", fontSize: 10, fontWeight: "900" }, wifiNetwork: { padding: 12, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white", flexDirection: "row", justifyContent: "space-between", alignItems: "center" }, wifiNetworkSelected: { borderColor: "#0a8c87", borderWidth: 2, backgroundColor: "#e9f7f5" }, signal: { color: "#59716f", fontSize: 10, fontWeight: "700" },
  dialogPage: { flex: 1, backgroundColor: "#f7faf9" }, dialogScreen: { flex: 1 }, dialogHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", paddingHorizontal: 22, paddingVertical: 18, borderBottomWidth: 1, borderBottomColor: "#dce6e5" }, stepLabel: { color: "#0a8c87", fontSize: 9, fontWeight: "900", letterSpacing: 1 }, dialogTitle: { color: "#0a3037", fontSize: 24, fontWeight: "900", marginTop: 3 }, close: { color: "#59716f", fontSize: 10, fontWeight: "900" }, dialogContent: { padding: 22, paddingBottom: 34, gap: 10 }, dialogHelp: { color: "#59716f", fontSize: 13, lineHeight: 19 }, selectedSummary: { padding: 13, borderRadius: 12, backgroundColor: "#e9f7f5", marginBottom: 4 }, fieldLabel: { color: "#385753", fontSize: 9, fontWeight: "900", letterSpacing: .9, marginTop: 5 }, fieldHint: { color: "#718783", fontSize: 10, marginTop: -5 }, backButton: { minHeight: 44, alignItems: "center", justifyContent: "center" }, dialogStatus: { color: "#59716f", fontSize: 11, textAlign: "center", marginTop: 2 }, dialogError: { color: "#b9472f", fontSize: 11, textAlign: "center" },
  sectionLabel: { color: "#7d9a97", fontSize: 10, fontWeight: "900", letterSpacing: 1.2, marginTop: 6 }, deviceCard: { padding: 18, borderRadius: 18, backgroundColor: "#f7faf9" }, deviceCardHeader: { flexDirection: "row", alignItems: "flex-start", justifyContent: "space-between" }, deviceCardName: { color: "#0a3037", fontSize: 18, fontWeight: "900" }, deviceSerial: { color: "#718783", fontSize: 10, fontWeight: "700", letterSpacing: .7, marginTop: 3 }, statusBadge: { flexDirection: "row", alignItems: "center", gap: 6, paddingHorizontal: 9, paddingVertical: 6, borderRadius: 20, backgroundColor: "#e7efed" }, statusDot: { width: 7, height: 7, borderRadius: 4 }, statusOnline: { backgroundColor: "#16a085" }, statusOffline: { backgroundColor: "#9badaa" }, statusText: { color: "#4d6965", fontSize: 8, fontWeight: "900", letterSpacing: .7 }, latestRow: { flexDirection: "row", alignItems: "flex-end", justifyContent: "space-between", marginTop: 24 }, latestLabel: { color: "#718783", fontSize: 8, fontWeight: "900", letterSpacing: .8 }, latestValue: { color: "#0a3037", fontSize: 39, lineHeight: 45, fontWeight: "900", letterSpacing: -1.5 }, cardArrow: { color: "#0a8c87", fontSize: 36, lineHeight: 42, fontWeight: "300" }, lastSeen: { color: "#718783", fontSize: 10, marginTop: 5 },
  detailPage: { flex: 1, backgroundColor: "#eef4f2" }, detailHeader: { minHeight: 58, paddingHorizontal: 20, flexDirection: "row", alignItems: "center", justifyContent: "space-between", borderBottomWidth: 1, borderBottomColor: "#d7e3e0" }, detailBack: { color: "#0a8c87", fontSize: 10, fontWeight: "900", letterSpacing: .8 }, detailSerial: { color: "#718783", fontSize: 9, fontWeight: "800", letterSpacing: .7 }, detailContent: { padding: 22, paddingBottom: 40, gap: 14 }, detailTitleRow: { flexDirection: "row", alignItems: "center", justifyContent: "space-between" }, detailEyebrow: { color: "#0a8c87", fontSize: 9, fontWeight: "900", letterSpacing: 1 }, detailTitle: { color: "#0a3037", fontSize: 30, fontWeight: "900", marginTop: 2 }, detailStatus: { flexDirection: "row", alignItems: "center", gap: 6 }, detailStatusText: { color: "#4d6965", fontSize: 9, fontWeight: "900", letterSpacing: .8 }, detailLatest: { padding: 19, borderRadius: 18, backgroundColor: "#0a3037" }, detailMetricLabel: { color: "#69cfc7", fontSize: 9, fontWeight: "900", letterSpacing: .9 }, detailMetricValue: { color: "white", fontSize: 45, lineHeight: 54, fontWeight: "900", letterSpacing: -1.5 }, detailMetricTime: { color: "#90aaa7", fontSize: 10 }, rangeTitle: { color: "#385753", fontSize: 9, fontWeight: "900", letterSpacing: .9, marginTop: 5 }, rangeSelector: { flexDirection: "row", padding: 4, borderRadius: 12, backgroundColor: "#dce8e5" }, rangeButton: { flex: 1, minHeight: 38, borderRadius: 9, alignItems: "center", justifyContent: "center" }, rangeButtonActive: { backgroundColor: "#0a8c87" }, rangeButtonText: { color: "#59716f", fontSize: 8, fontWeight: "900" }, rangeButtonTextActive: { color: "white" }, chartCard: { padding: 16, borderRadius: 18, backgroundColor: "white" }, chartCardHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", marginBottom: 8 }, chartTitle: { color: "#0a3037", fontSize: 17, fontWeight: "900" }, chartSubtitle: { color: "#718783", fontSize: 9, marginTop: 2 }, chart: { height: 210, overflow: "hidden", borderRadius: 12, backgroundColor: "#f3f8f6" }, chartGrid: { position: "absolute", left: 18, right: 18, height: 1, backgroundColor: "#dce8e5" }, chartLine: { position: "absolute", height: 2, borderRadius: 1, backgroundColor: "#0a8c87" }, chartDot: { position: "absolute", width: 8, height: 8, borderRadius: 4, backgroundColor: "#ff8264", borderWidth: 2, borderColor: "white" }, chartEmpty: { color: "#718783", fontSize: 11, textAlign: "center", marginTop: 95 }, chartMax: { position: "absolute", top: 4, right: 6, color: "#718783", fontSize: 8, fontWeight: "800" }, chartMin: { position: "absolute", bottom: 4, right: 6, color: "#718783", fontSize: 8, fontWeight: "800" }, chartAxis: { flexDirection: "row", justifyContent: "space-between", marginTop: 6 }, chartAxisText: { color: "#718783", fontSize: 8, fontWeight: "800" }, historyError: { color: "#b9472f", fontSize: 10, textAlign: "center", marginTop: 8 }, statsRow: { flexDirection: "row", gap: 10 }, stat: { flex: 1, padding: 13, borderRadius: 14, backgroundColor: "white" }, statLabel: { color: "#718783", fontSize: 8, fontWeight: "900", letterSpacing: .7 }, statValue: { color: "#0a3037", fontSize: 19, fontWeight: "900", marginTop: 4 }, sensorNote: { color: "#718783", fontSize: 10, lineHeight: 15, textAlign: "center" }, otaZone: { padding: 16, borderWidth: 1, borderColor: "#b8d9d5", borderRadius: 14, backgroundColor: "#f4fbfa" }, otaTitle: { color: "#0a6f6b", fontSize: 9, fontWeight: "900", letterSpacing: .8 }, otaDescription: { color: "#59716f", fontSize: 10, lineHeight: 15, marginTop: 5, marginBottom: 11 }, otaButton: { minHeight: 46, borderRadius: 11, alignItems: "center", justifyContent: "center", backgroundColor: "#0a8c87" }, otaButtonText: { color: "white", fontSize: 9, fontWeight: "900", letterSpacing: .8 }, dangerZone: { padding: 16, borderWidth: 1, borderColor: "#e8b5aa", borderRadius: 14, backgroundColor: "#fff4f1" }, dangerTitle: { color: "#8f3422", fontSize: 9, fontWeight: "900", letterSpacing: .8 }, dangerDescription: { color: "#7f5b53", fontSize: 10, lineHeight: 15, marginTop: 5, marginBottom: 11 }, deleteButton: { minHeight: 46, borderRadius: 11, alignItems: "center", justifyContent: "center", backgroundColor: "#b9472f" }, deleteButtonText: { color: "white", fontSize: 9, fontWeight: "900", letterSpacing: .8 },
  topologyCard: { padding: 16, borderRadius: 18, backgroundColor: "white", gap: 12 }, topologyHeader: { flexDirection: "row", alignItems: "flex-start", justifyContent: "space-between", gap: 8 }, topologyCount: { color: "#0a8c87", fontSize: 8, fontWeight: "900", letterSpacing: .7 }, topologyRoot: { flexDirection: "row", alignItems: "center", gap: 10, padding: 11, borderRadius: 12, backgroundColor: "#e9f7f5" }, topologyRootIcon: { width: 38, height: 38, borderRadius: 19, alignItems: "center", justifyContent: "center", backgroundColor: "#0a3037" }, topologyNodeName: { color: "#0a3037", fontSize: 12, fontWeight: "900" }, topologyNodeMeta: { color: "#718783", fontSize: 8, marginTop: 2 }, topologyLinkRow: { minHeight: 52, flexDirection: "row", alignItems: "center" }, topologyRail: { width: 50, alignSelf: "stretch", position: "relative" }, topologyVertical: { position: "absolute", left: 18, top: -12, bottom: 26, width: 2, backgroundColor: "#8fcac4" }, topologyHorizontal: { position: "absolute", left: 18, top: 25, width: 25, height: 2, backgroundColor: "#8fcac4" }, topologyPeerDot: { position: "absolute", left: 39, top: 19, width: 14, height: 14, borderRadius: 7, borderWidth: 3, borderColor: "#0a8c87", backgroundColor: "white" }, topologyPeerInfo: { flex: 1, paddingVertical: 7, paddingHorizontal: 10, borderRadius: 10, backgroundColor: "#f3f8f6" }, topologyEmpty: { paddingVertical: 18, color: "#718783", fontSize: 10, textAlign: "center" },
  empty: { color: "#829d9a", textAlign: "center", padding: 28 }, error: { color: "#ffc0af", textAlign: "center", fontSize: 12, paddingVertical: 12 },
});
