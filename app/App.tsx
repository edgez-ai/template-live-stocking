import Constants from "expo-constants";
import { StatusBar } from "expo-status-bar";
import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { ActivityIndicator, Alert, KeyboardAvoidingView, Modal, PermissionsAndroid, Platform, Pressable, ScrollView, StyleSheet, Text, TextInput, View } from "react-native";
import type { LayoutChangeEvent } from "react-native";
import { SafeAreaProvider, SafeAreaView } from "react-native-safe-area-context";
import { Account, Client, ID, Models, Permission, Query, Role, TablesDB, Teams } from "react-native-appwrite";
import { ESPDevice, ESPProvisionManager, ESPSecurity, ESPTransport } from "@orbital-systems/react-native-esp-idf-provisioning";
import type { ESPWifiList } from "@orbital-systems/react-native-esp-idf-provisioning";
import { EdgezOrganicMap } from "@edgez/react-native-sdk";
import type { EdgezMapDownloadUpdate, EdgezMapNode, EdgezOrganicMapRef } from "@edgez/react-native-sdk";
import Ionicons from "@expo/vector-icons/Ionicons";
import AsyncStorage from "@react-native-async-storage/async-storage";
import * as Location from "expo-location";
import { centerChannelForCountry, channelsForCountry, halowCountries } from "./halowChannels";

type Device = { $id: string; serial: string; name: string; status: string; enabled: boolean; metadata?: { farmId?: string; latitude?: number; longitude?: number; [key: string]: unknown }; latitude?: number; longitude?: number };
type Farm = Models.Row & { name: string; country: string; location: string; halowChannel: number; meshId: string; meshPassphrase: string; teamId: string; ownerId: string };
type CurrentUser = Pick<Models.User<Models.Preferences>, "$id" | "email" | "prefs">;
type CachedFarm = Pick<Farm, "$id" | "name" | "country" | "location" | "halowChannel" | "meshId" | "teamId" | "ownerId">;
type CachedSnapshot = { version: 1; user: CurrentUser; farms: CachedFarm[]; devices: Device[]; telemetry: CachedTelemetry[] };
type FarmDetails = { name: string; country: string; location: string; halowChannel: string; meshId: string; meshPassphrase: string };
type Credential = { clientId: string; username: string; password: string };
type Telemetry = Models.Row & { deviceId: string; serial: string; channel: string; topic: string; payload: string; receivedAt: string };
type CachedTelemetry = Pick<Telemetry, "$id" | "deviceId" | "serial" | "channel" | "topic" | "payload" | "receivedAt">;
type AppConfig = { appwriteEndpoint: string; appwriteProjectId: string; appwritePlatform: string; databaseId: string; telemetryTableId: string; farmTableId: string };
type HistoryRange = "30m" | "1h" | "6h" | "24h";
type DashboardView = "map" | "list";
type DeviceLocationChoice = "none" | "current" | "map";
type VoltagePoint = { timestamp: number; value: number };
const emptyFarmDetails: FarmDetails = { name: "", country: "", location: "", halowChannel: "", meshId: "", meshPassphrase: "" };

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

function telemetryCoordinates(row?: Telemetry) {
  if (!row) return null;
  let payload: { latitude?: unknown; longitude?: unknown };
  try { payload = JSON.parse(row.payload); } catch { return null; }
  const latitude = payload.latitude;
  const longitude = payload.longitude;
  return typeof latitude === "number" && typeof longitude === "number" &&
    Number.isFinite(latitude) && Number.isFinite(longitude) && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180
    ? { latitude, longitude } : null;
}

const appConfig = Constants.expoConfig?.extra as AppConfig | undefined;
if (!appConfig) throw new Error("Expo Appwrite configuration is missing");
const config: AppConfig = appConfig;
const endpoint = config.appwriteEndpoint.replace(/\/+$/, "");
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
    ({ $id, serial, name, status, enabled, metadata: { farmId: metadata?.farmId } }));
  const safeTelemetry: CachedTelemetry[] = telemetry.map(({ $id, deviceId, serial, channel, topic, payload, receivedAt }) =>
    ({ $id, deviceId, serial, channel, topic, payload, receivedAt }));
  const safeUser: CurrentUser = { $id: user.$id, email: user.email, prefs: { currentFarmId: (user.prefs as { currentFarmId?: string }).currentFarmId } };
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
    const value = Number((JSON.parse(row.payload) as { batteryVoltageMv?: unknown }).batteryVoltageMv);
    return (row.channel === "status" || row.channel === "battery") && Number.isInteger(value) && value >= 2500 && value <= 5000 ? value / 1000 : null;
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

function OfflineMap({ devices, telemetry, location }: { devices: Device[]; telemetry: Telemetry[]; location?: string }) {
  const map = useRef<EdgezOrganicMapRef>(null);
  const [region, setRegion] = useState("");
  const [download, setDownload] = useState<EdgezMapDownloadUpdate | null>(null);
  const [mapError, setMapError] = useState("");
  const farmCenter = coordinatesFromLocation(location ?? "");
  const markers = useMemo<EdgezMapNode[]>(() => devices.flatMap((device) => {
    const coordinates = telemetry.map((row) => row.deviceId === device.$id ? telemetryCoordinates(row) : null).find(Boolean);
    return coordinates ? [{ id: device.$id, label: device.name, ...coordinates, marker: device.enabled ? "blue" : "gray" }] : [];
  }), [devices, telemetry]);

  return <View style={styles.mapCard}>
    <EdgezOrganicMap
      ref={map}
      nodes={markers}
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
    <Text style={styles.mapAttribution}>{markers.length ? `${markers.length} located devices` : "Pan or zoom to choose an offline region"} · © OpenStreetMap contributors</Text>
  </View>;
}

function serialFromBleName(name: string) {
  const serial = name.startsWith("PROV_") ? name.slice(5).toUpperCase() : "";
  if (!/^[A-F0-9]{12}$/.test(serial)) throw new Error(`Invalid provisioning name: ${name}`);
  return serial;
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
  const [locationPickerFor, setLocationPickerFor] = useState<"new" | "edit" | null>(null);
  const [newFarm, setNewFarm] = useState<FarmDetails>(emptyFarmDetails);
  const [farmDraft, setFarmDraft] = useState<FarmDetails>(emptyFarmDetails);
  const [telemetry, setTelemetry] = useState<Telemetry[]>([]);
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [bleDevices, setBleDevices] = useState<ESPDevice[]>([]);
  const [selectedBleDevice, setSelectedBleDevice] = useState<ESPDevice | null>(null);
  const [proofOfPossession, setProofOfPossession] = useState(provisioningPop);
  const [bleConnected, setBleConnected] = useState(false);
  const [name, setName] = useState("");
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
      const [farmResult, deviceResult, telemetryResult] = await Promise.allSettled([
        tables.listRows<Farm>({ databaseId: config.databaseId, tableId: config.farmTableId, queries: [Query.limit(100)] }),
        deviceApi<{ devices: Device[] }>(),
        tables.listRows({ databaseId: config.databaseId, tableId: config.telemetryTableId, queries: [Query.orderDesc("receivedAt"), Query.limit(500)] }),
      ]);
      const failures = [farmResult, deviceResult, telemetryResult].filter((result): result is PromiseRejectedResult => result.status === "rejected");
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
          setFarms([]); setDevices([]); setTelemetry([]);
          setCurrentFarmId((current.prefs as { currentFarmId?: string }).currentFarmId || "");
        }
        setUser(current);
        const availableFarms = await refresh(current);
        if (active && !availableFarms.length) setSettingsOpen(true);
      } catch (caught) {
        if (!active) return;
        if (isAuthError(caught)) {
          if (cached) await clearCachedUser(cached.user.$id);
          activeUserId.current = null;
          farmsRef.current = []; devicesRef.current = []; telemetryRef.current = [];
          setUser(null); setFarms([]); setDevices([]); setTelemetry([]); setCurrentFarmId(""); setOffline(false);
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
          setUser(null); setFarms([]); setDevices([]); setTelemetry([]); setCurrentFarmId(""); setOffline(false);
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
      setCurrentFarmId((current.prefs as { currentFarmId?: string }).currentFarmId || "");
      const availableFarms = await refresh(current);
      if (!availableFarms.length) setSettingsOpen(true);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function scanBleDevices() {
    setBusy(true); setError(""); setProvisioningStatus("Scanning for PROV_ devices…");
    try {
      selectedBleDevice?.disconnect();
      setSelectedBleDevice(null); setProofOfPossession(provisioningPop); setBleConnected(false);
      await requestBlePermissions();
      const found = await ESPProvisionManager.searchESPDevices("PROV_", ESPTransport.ble, ESPSecurity.secure);
      const valid = found.filter((device) => /^PROV_[A-F0-9]{12}$/i.test(device.name));
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
    setBleDevices([]); setSelectedBleDevice(null); setProofOfPossession(provisioningPop); setBleConnected(false); setName(""); setDeviceLocationChoice("none"); setDeviceLocation(""); setDeviceLocationPickerOpen(false); setUseUpstreamWifi(null); setUpstreamSsid(""); setUpstreamPassword(""); setUpstreamNetworks([]);
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
    if (provisioningStep === 4) { setProvisioningStep(3); return; }
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

  function selectBleDevice(device: ESPDevice) {
    selectedBleDevice?.disconnect();
    setError("");
    setSelectedBleDevice(device);
    const existing = devices.find((item) => item.serial === serialFromBleName(device.name));
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
    if (!selectedBleDevice || !proofOfPossession.trim()) return;
    setBusy(true); setError("");
    try {
      setProvisioningStatus(`Authenticating ${selectedBleDevice.name} with the provided PoP…`);
      await selectedBleDevice.connect(proofOfPossession.trim());
      setBleConnected(true);
      setProvisioningStatus("Choose whether this device has an upstream Wi-Fi connection.");
      setProvisioningStep(3);
    } catch (caught) {
      selectedBleDevice.disconnect();
      setBleConnected(false);
      setError(messageOf(caught));
      setProvisioningStatus("BLE connection failed. Check the ESP32 and PoP, then retry.");
    } finally { setBusy(false); }
  }

  async function chooseUpstreamWifi(enabled: boolean) {
    setUseUpstreamWifi(enabled);
    setUpstreamSsid(""); setUpstreamPassword(""); setUpstreamNetworks([]);
    if (!enabled || !selectedBleDevice) return;
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
      const coordinates = deviceLocationChoice === "none" ? null : coordinatesFromLocation(deviceLocation);
      if (deviceLocationChoice !== "none" && !coordinates) throw new Error("Choose a valid device location before provisioning.");
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
          metadata: { farmId: farm.$id },
        });
      }
      const mqtt = await deviceApi<Credential>(`/${encodeURIComponent(appwriteDevice.$id)}/credentials`, "POST", {});

      setProvisioningStatus(`Sending ${farm.name} device configuration…`);
      const mqttResponse = await selectedBleDevice.sendData("mqtt-config", JSON.stringify({
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
        ...(coordinates ?? { latitude: null, longitude: null }),
      }));
      const accepted = JSON.parse(mqttResponse) as { ok?: boolean; error?: string };
      if (!accepted.ok) throw new Error(accepted.error || "The device rejected its configuration.");
      if (useUpstreamWifi) {
        setProvisioningStatus("Connecting the ESP32 to upstream Wi-Fi…");
        await selectedBleDevice.provision(upstreamSsid, upstreamPassword);
      }
      setProvisioningStatus(`Provisioned ${serial}. Waiting for battery telemetry.`);
      setSelectedBleDevice(null); setBleDevices([]); setProofOfPossession(provisioningPop); setBleConnected(false); setName(""); setDeviceLocationChoice("none"); setDeviceLocation("");
      setProvisioningDialogOpen(false);
      await refresh(user);
    } catch (caught) { setError(messageOf(caught)); setProvisioningStatus("Provisioning did not complete."); }
    finally { selectedBleDevice.disconnect(); setBleConnected(false); setBusy(false); }
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
    setUser(null); setDevices([]); setFarms([]); setCurrentFarmId(""); setSettingsOpen(false); setLocationPickerFor(null); setTelemetry([]); setBleDevices([]); setSelectedBleDevice(null); setProofOfPossession(provisioningPop); setBleConnected(false); setProvisioningDialogOpen(false); setDetailDevice(null); setDashboardView("map"); setOffline(false);
  }

  function openSettings() {
    setMenuOpen(false);
    setLocationPickerFor(null);
    setFarmDraft(detailsFromFarm(farms.find((farm) => farm.$id === currentFarmId)));
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
      setFarmDraft(detailsFromFarm(farm));
      setDetailDevice(null);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function createFarm() {
    if (!user || offline) return;
    setBusy(true); setError("");
    const teamId = ID.unique();
    try {
      const data = farmData(newFarm);
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
      setNewFarm(emptyFarmDetails);
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
    } catch (caught) { setError(messageOf(caught)); }
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
  const currentFarm = farms.find((farm) => farm.$id === currentFarmId);
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
      {dashboardView === "map" ? <OfflineMap key={`${currentFarm?.$id ?? "none"}:${currentFarm?.location ?? ""}`} devices={visibleDevices} telemetry={telemetry} location={currentFarm?.location} /> : <ScrollView style={styles.listScroll} contentContainerStyle={styles.listContent} keyboardShouldPersistTaps="handled">
      <Text style={styles.sectionLabel}>{visibleDevices.length} DEVICES</Text>
      {visibleDevices.map((device) => {
        const latest = latestVoltageByDevice.get(device.$id);
        const status = statusOf(device, latestTelemetryByDevice.get(device.$id));
        return <Pressable key={device.$id} style={styles.deviceCard} onPress={() => { setHistoryRange("1h"); setDetailDevice(device); }}>
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
          <Pressable style={styles.menuItem} onPress={() => { setDashboardView("map"); setMenuOpen(false); }} accessibilityRole="menuitem"><Text style={[styles.menuItemText, dashboardView === "map" && styles.menuItemActive]}>Map view</Text></Pressable>
          <Pressable style={styles.menuItem} onPress={() => { setDashboardView("list"); setMenuOpen(false); }} accessibilityRole="menuitem"><Text style={[styles.menuItemText, dashboardView === "list" && styles.menuItemActive]}>List view</Text></Pressable>
          <Pressable style={styles.menuItem} onPress={openSettings} accessibilityRole="menuitem"><Text style={styles.menuItemText}>Settings · Farms</Text></Pressable>
          <View style={styles.menuDivider} />
          <Pressable style={styles.menuItem} onPress={() => void signOut().catch((caught) => setError(messageOf(caught)))} accessibilityRole="menuitem"><Text style={styles.menuItemText}>Sign out</Text></Pressable>
        </View>}
      </View>
    </View>}
    <Modal visible={settingsOpen && Boolean(user)} animationType="slide" onRequestClose={() => locationPickerFor ? setLocationPickerFor(null) : setSettingsOpen(false)}>
      {locationPickerFor ? <FarmLocationPicker location={locationPickerFor === "new" ? newFarm.location : farmDraft.location} country={locationPickerFor === "new" ? newFarm.country : farmDraft.country} onCancel={() => setLocationPickerFor(null)} onSelect={(location) => { if (locationPickerFor === "new") setNewFarm({ ...newFarm, location }); else setFarmDraft({ ...farmDraft, location }); setLocationPickerFor(null); }} /> : <SafeAreaView style={styles.dialogPage}>
        <View style={styles.dialogHeader}><View><Text style={styles.stepLabel}>SETTINGS</Text><Text style={styles.dialogTitle}>Farms</Text></View><Pressable onPress={() => setSettingsOpen(false)}><Text style={styles.close}>CLOSE</Text></Pressable></View>
        <ScrollView contentContainerStyle={styles.dialogContent} keyboardShouldPersistTaps="handled">
          <Text style={styles.dialogHelp}>Choose the farm for this app. The selected farm is saved to your account and opens next time.</Text>
          {offline && <Text style={styles.dialogHelp}>Offline: cached farms can be viewed and switched. Connect to edit or create a farm.</Text>}
          {farms.map((farm) => <Pressable key={farm.$id} style={[styles.farmRow, farm.$id === currentFarmId && styles.farmRowSelected]} onPress={() => void selectFarm(farm)} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: farm.$id === currentFarmId }}><Text style={styles.deviceNameDark}>{farm.name}</Text><Text style={styles.muted}>{farm.$id === currentFarmId ? "CURRENT FARM" : "TAP TO SWITCH"}</Text></Pressable>)}
          {!farms.length && <Text style={styles.muted}>No farms yet. Create the first farm below.</Text>}
          <Text style={styles.fieldLabel}>CREATE FARM</Text>
          <FarmFields value={newFarm} onChange={setNewFarm} onPickLocation={() => setLocationPickerFor("new")} />
          <Pressable style={[styles.primary, offline && styles.disabledButton]} onPress={() => void createFarm()} disabled={busy || offline}><Text style={styles.primaryText}>CREATE FARM & TEAM</Text></Pressable>
          {currentFarm && currentFarm.ownerId === user?.$id && <>
            <Text style={styles.fieldLabel}>EDIT CURRENT FARM</Text>
            <FarmFields value={farmDraft} onChange={setFarmDraft} onPickLocation={() => setLocationPickerFor("edit")} />
            <Pressable style={[styles.primary, offline && styles.disabledButton]} onPress={() => void saveFarm()} disabled={busy || offline}><Text style={styles.primaryText}>SAVE FARM</Text></Pressable>
          </>}
          {error ? <Text style={styles.dialogError}>{error}</Text> : null}
        </ScrollView>
      </SafeAreaView>}
    </Modal>
    <Modal visible={provisioningDialogOpen} animationType="slide" onRequestClose={() => deviceLocationPickerOpen ? setDeviceLocationPickerOpen(false) : closeProvisioningDialog()}>
      {deviceLocationPickerOpen ? <FarmLocationPicker device location={deviceLocation || currentFarm?.location || ""} country={currentFarm?.country || ""} onCancel={() => setDeviceLocationPickerOpen(false)} onSelect={(location) => { setDeviceLocation(location); setDeviceLocationChoice("map"); setDeviceLocationPickerOpen(false); }} /> : <SafeAreaView style={styles.dialogPage}>
        <KeyboardAvoidingView style={styles.dialogScreen} behavior={Platform.OS === "ios" ? "padding" : undefined} accessibilityViewIsModal>
          <View style={styles.dialogHeader}><View><Text style={styles.stepLabel}>STEP {provisioningStep} OF 4</Text><Text style={styles.dialogTitle}>{provisioningStep === 1 ? "Choose device" : provisioningStep === 2 ? "Device details" : provisioningStep === 3 ? "Upstream Wi-Fi" : "Confirm setup"}</Text></View><Pressable onPress={closeProvisioningDialog} disabled={busy}><Text style={styles.close}>CLOSE</Text></Pressable></View>
          <ScrollView contentContainerStyle={styles.dialogContent} keyboardShouldPersistTaps="handled">
            {provisioningStep === 1 && <>
              <Text style={styles.dialogHelp}>Put the ESP32 in provisioning mode, then scan for its PROV_ Bluetooth name.</Text>
              <Pressable style={styles.primary} onPress={scanBleDevices} disabled={busy}><Text style={styles.primaryText}>{busy ? "SCANNING…" : "SCAN FOR DEVICES"}</Text></Pressable>
              {bleDevices.map((device) => <Pressable key={device.name} style={styles.bleDevice} onPress={() => selectBleDevice(device)} disabled={busy}><Text style={styles.deviceNameDark}>{device.name}</Text><Text style={styles.muted}>Serial {device.name.slice(5).toUpperCase()}</Text></Pressable>)}
            </>}
            {provisioningStep === 2 && selectedBleDevice && <>
              <View style={styles.selectedSummary}><Text style={styles.deviceNameDark}>{selectedBleDevice.name}</Text><Text style={styles.muted}>Serial {selectedBleDevice.name.slice(5).toUpperCase()}</Text></View>
              <Text style={styles.fieldLabel}>NAME</Text>
              <TextInput style={styles.inputLight} value={name} onChangeText={setName} placeholder="Device name" maxLength={128} />
              <Text style={styles.fieldHint}>Optional. The serial is used when no name is entered.</Text>
              <Text style={styles.fieldLabel}>LOCATION · OPTIONAL</Text>
              <Pressable style={[styles.farmRow, deviceLocationChoice === "none" && styles.farmRowSelected]} onPress={() => { setDeviceLocationChoice("none"); setDeviceLocation(""); }} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: deviceLocationChoice === "none" }}><Text style={styles.deviceNameDark}>None</Text></Pressable>
              <Pressable style={[styles.farmRow, deviceLocationChoice === "current" && styles.farmRowSelected]} onPress={() => void chooseCurrentDeviceLocation()} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: deviceLocationChoice === "current" }}><Text style={styles.deviceNameDark}>{busy ? "Finding current location…" : "Current location"}</Text>{deviceLocationChoice === "current" && <Text style={styles.muted}>{deviceLocation}</Text>}</Pressable>
              <Pressable style={[styles.farmRow, deviceLocationChoice === "map" && styles.farmRowSelected]} onPress={() => setDeviceLocationPickerOpen(true)} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: deviceLocationChoice === "map" }}><Text style={styles.deviceNameDark}>Choose on map</Text>{deviceLocationChoice === "map" && <Text style={styles.muted}>{deviceLocation}</Text>}</Pressable>
              <Text style={styles.fieldLabel}>PROOF OF POSSESSION (PoP)</Text>
              <TextInput style={styles.inputLight} value={proofOfPossession} onChangeText={setProofOfPossession} placeholder="PoP shown on the device OLED" autoCapitalize="none" autoCorrect={false} />
              <Pressable style={styles.primary} onPress={connectForProvisioning} disabled={busy || !proofOfPossession.trim()}><Text style={styles.primaryText}>{busy ? "CONNECTING…" : "CONNECT DEVICE"}</Text></Pressable>
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
              <View style={styles.selectedSummary}><Text style={styles.deviceNameDark}>{currentFarm?.name}</Text><Text style={styles.muted}>{currentFarm?.location}, {currentFarm?.country} · Channel {currentFarm?.halowChannel}</Text><Text style={styles.muted}>Mesh ID: {currentFarm?.meshId}</Text><Text style={styles.muted}>Device location: {deviceLocationChoice === "none" ? "None" : deviceLocation}</Text></View>
              <Text style={styles.dialogHelp}>{useUpstreamWifi ? `Upstream Wi-Fi: ${upstreamSsid}. ` : "No upstream Wi-Fi. "}The app will send the mesh settings and MQTT credential to the device.</Text>
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
            <View style={styles.detailLatest}><Text style={styles.detailMetricLabel}>BATTERY VOLTAGE</Text><Text style={styles.detailMetricValue}>{detailLatest ? `${detailLatest.value.toFixed(2)} V` : "—"}</Text><Text style={styles.detailMetricTime}>{detailLatest ? `Updated ${relativeTime(detailLatest.row.receivedAt)}` : "No readings received"}</Text></View>
            <Text style={styles.rangeTitle}>HISTORY RANGE</Text>
            <View style={styles.rangeSelector}>{historyRanges.map((range) => <Pressable key={range.key} style={[styles.rangeButton, historyRange === range.key && styles.rangeButtonActive]} onPress={() => setHistoryRange(range.key)} disabled={historyLoading}><Text style={[styles.rangeButtonText, historyRange === range.key && styles.rangeButtonTextActive]}>{range.label}</Text></Pressable>)}</View>
            <View style={styles.chartCard}><View style={styles.chartCardHeader}><View><Text style={styles.chartTitle}>Battery voltage</Text><Text style={styles.chartSubtitle}>HT-HC33 battery ADC · {historyPoints.length} readings{offline ? " · cached" : ""}</Text></View>{historyLoading ? <ActivityIndicator color="#0a8c87" /> : null}</View><VoltageChart points={historyPoints} duration={activeHistoryDuration} /><View style={styles.chartAxis}><Text style={styles.chartAxisText}>{activeHistoryRange.label} AGO</Text><Text style={styles.chartAxisText}>NOW</Text></View>{historyError ? <Text style={styles.historyError}>{historyError}</Text> : null}</View>
            {historyStats ? <View style={styles.statsRow}><View style={styles.stat}><Text style={styles.statLabel}>MIN</Text><Text style={styles.statValue}>{historyStats.min.toFixed(2)} V</Text></View><View style={styles.stat}><Text style={styles.statLabel}>AVERAGE</Text><Text style={styles.statValue}>{historyStats.average.toFixed(2)} V</Text></View><View style={styles.stat}><Text style={styles.statLabel}>MAX</Text><Text style={styles.statValue}>{historyStats.max.toFixed(2)} V</Text></View></View> : null}
            <Text style={styles.sensorNote}>Battery voltage is measured on the HT-HC33; no reading appears when a battery is disconnected.</Text>
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
  farmRow: { padding: 16, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white", gap: 4 }, farmRowSelected: { borderColor: "#0a8c87", borderWidth: 2, backgroundColor: "#e9f7f5" },
  selectField: { minHeight: 54, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 12, paddingHorizontal: 15, backgroundColor: "white", flexDirection: "row", justifyContent: "space-between", alignItems: "center", gap: 8 }, selectText: { color: "#0a3037", fontSize: 14 }, optionList: { borderWidth: 1, borderColor: "#cedbdc", borderRadius: 12, backgroundColor: "white", overflow: "hidden" }, optionRow: { minHeight: 46, paddingHorizontal: 15, justifyContent: "center", borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: "#dce6e5" },
  passphraseField: { height: 54, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 12, backgroundColor: "white", flexDirection: "row", alignItems: "center" }, passphraseInput: { flex: 1, height: 52, paddingLeft: 15, color: "#0a3037" }, visibilityButton: { width: 54, height: 52, alignItems: "center", justifyContent: "center" },
  locationMap: { flex: 1, overflow: "hidden", backgroundColor: "#dce8e5" }, mapCrosshair: { position: "absolute", left: "50%", top: "50%", marginLeft: -18, marginTop: -26, width: 36, height: 52, alignItems: "center", justifyContent: "center" }, mapCrosshairText: { color: "#0a8c87", fontSize: 42, fontWeight: "900", textShadowColor: "white", textShadowRadius: 4 }, locationFooter: { paddingHorizontal: 22, paddingVertical: 12, gap: 5, backgroundColor: "#f7faf9" },
  bleDevice: { padding: 13, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white" }, deviceNameDark: { color: "#0a3037", fontSize: 14, fontWeight: "800" },
  wifiHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", marginTop: 4 }, rescan: { color: "#0a8c87", fontSize: 10, fontWeight: "900" }, wifiNetwork: { padding: 12, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white", flexDirection: "row", justifyContent: "space-between", alignItems: "center" }, wifiNetworkSelected: { borderColor: "#0a8c87", borderWidth: 2, backgroundColor: "#e9f7f5" }, signal: { color: "#59716f", fontSize: 10, fontWeight: "700" },
  dialogPage: { flex: 1, backgroundColor: "#f7faf9" }, dialogScreen: { flex: 1 }, dialogHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", paddingHorizontal: 22, paddingVertical: 18, borderBottomWidth: 1, borderBottomColor: "#dce6e5" }, stepLabel: { color: "#0a8c87", fontSize: 9, fontWeight: "900", letterSpacing: 1 }, dialogTitle: { color: "#0a3037", fontSize: 24, fontWeight: "900", marginTop: 3 }, close: { color: "#59716f", fontSize: 10, fontWeight: "900" }, dialogContent: { padding: 22, paddingBottom: 34, gap: 10 }, dialogHelp: { color: "#59716f", fontSize: 13, lineHeight: 19 }, selectedSummary: { padding: 13, borderRadius: 12, backgroundColor: "#e9f7f5", marginBottom: 4 }, fieldLabel: { color: "#385753", fontSize: 9, fontWeight: "900", letterSpacing: .9, marginTop: 5 }, fieldHint: { color: "#718783", fontSize: 10, marginTop: -5 }, backButton: { minHeight: 44, alignItems: "center", justifyContent: "center" }, dialogStatus: { color: "#59716f", fontSize: 11, textAlign: "center", marginTop: 2 }, dialogError: { color: "#b9472f", fontSize: 11, textAlign: "center" },
  sectionLabel: { color: "#7d9a97", fontSize: 10, fontWeight: "900", letterSpacing: 1.2, marginTop: 6 }, deviceCard: { padding: 18, borderRadius: 18, backgroundColor: "#f7faf9" }, deviceCardHeader: { flexDirection: "row", alignItems: "flex-start", justifyContent: "space-between" }, deviceCardName: { color: "#0a3037", fontSize: 18, fontWeight: "900" }, deviceSerial: { color: "#718783", fontSize: 10, fontWeight: "700", letterSpacing: .7, marginTop: 3 }, statusBadge: { flexDirection: "row", alignItems: "center", gap: 6, paddingHorizontal: 9, paddingVertical: 6, borderRadius: 20, backgroundColor: "#e7efed" }, statusDot: { width: 7, height: 7, borderRadius: 4 }, statusOnline: { backgroundColor: "#16a085" }, statusOffline: { backgroundColor: "#9badaa" }, statusText: { color: "#4d6965", fontSize: 8, fontWeight: "900", letterSpacing: .7 }, latestRow: { flexDirection: "row", alignItems: "flex-end", justifyContent: "space-between", marginTop: 24 }, latestLabel: { color: "#718783", fontSize: 8, fontWeight: "900", letterSpacing: .8 }, latestValue: { color: "#0a3037", fontSize: 39, lineHeight: 45, fontWeight: "900", letterSpacing: -1.5 }, cardArrow: { color: "#0a8c87", fontSize: 36, lineHeight: 42, fontWeight: "300" }, lastSeen: { color: "#718783", fontSize: 10, marginTop: 5 },
  detailPage: { flex: 1, backgroundColor: "#eef4f2" }, detailHeader: { minHeight: 58, paddingHorizontal: 20, flexDirection: "row", alignItems: "center", justifyContent: "space-between", borderBottomWidth: 1, borderBottomColor: "#d7e3e0" }, detailBack: { color: "#0a8c87", fontSize: 10, fontWeight: "900", letterSpacing: .8 }, detailSerial: { color: "#718783", fontSize: 9, fontWeight: "800", letterSpacing: .7 }, detailContent: { padding: 22, paddingBottom: 40, gap: 14 }, detailTitleRow: { flexDirection: "row", alignItems: "center", justifyContent: "space-between" }, detailEyebrow: { color: "#0a8c87", fontSize: 9, fontWeight: "900", letterSpacing: 1 }, detailTitle: { color: "#0a3037", fontSize: 30, fontWeight: "900", marginTop: 2 }, detailStatus: { flexDirection: "row", alignItems: "center", gap: 6 }, detailStatusText: { color: "#4d6965", fontSize: 9, fontWeight: "900", letterSpacing: .8 }, detailLatest: { padding: 19, borderRadius: 18, backgroundColor: "#0a3037" }, detailMetricLabel: { color: "#69cfc7", fontSize: 9, fontWeight: "900", letterSpacing: .9 }, detailMetricValue: { color: "white", fontSize: 45, lineHeight: 54, fontWeight: "900", letterSpacing: -1.5 }, detailMetricTime: { color: "#90aaa7", fontSize: 10 }, rangeTitle: { color: "#385753", fontSize: 9, fontWeight: "900", letterSpacing: .9, marginTop: 5 }, rangeSelector: { flexDirection: "row", padding: 4, borderRadius: 12, backgroundColor: "#dce8e5" }, rangeButton: { flex: 1, minHeight: 38, borderRadius: 9, alignItems: "center", justifyContent: "center" }, rangeButtonActive: { backgroundColor: "#0a8c87" }, rangeButtonText: { color: "#59716f", fontSize: 8, fontWeight: "900" }, rangeButtonTextActive: { color: "white" }, chartCard: { padding: 16, borderRadius: 18, backgroundColor: "white" }, chartCardHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", marginBottom: 8 }, chartTitle: { color: "#0a3037", fontSize: 17, fontWeight: "900" }, chartSubtitle: { color: "#718783", fontSize: 9, marginTop: 2 }, chart: { height: 210, overflow: "hidden", borderRadius: 12, backgroundColor: "#f3f8f6" }, chartGrid: { position: "absolute", left: 18, right: 18, height: 1, backgroundColor: "#dce8e5" }, chartLine: { position: "absolute", height: 2, borderRadius: 1, backgroundColor: "#0a8c87" }, chartDot: { position: "absolute", width: 8, height: 8, borderRadius: 4, backgroundColor: "#ff8264", borderWidth: 2, borderColor: "white" }, chartEmpty: { color: "#718783", fontSize: 11, textAlign: "center", marginTop: 95 }, chartMax: { position: "absolute", top: 4, right: 6, color: "#718783", fontSize: 8, fontWeight: "800" }, chartMin: { position: "absolute", bottom: 4, right: 6, color: "#718783", fontSize: 8, fontWeight: "800" }, chartAxis: { flexDirection: "row", justifyContent: "space-between", marginTop: 6 }, chartAxisText: { color: "#718783", fontSize: 8, fontWeight: "800" }, historyError: { color: "#b9472f", fontSize: 10, textAlign: "center", marginTop: 8 }, statsRow: { flexDirection: "row", gap: 10 }, stat: { flex: 1, padding: 13, borderRadius: 14, backgroundColor: "white" }, statLabel: { color: "#718783", fontSize: 8, fontWeight: "900", letterSpacing: .7 }, statValue: { color: "#0a3037", fontSize: 19, fontWeight: "900", marginTop: 4 }, sensorNote: { color: "#718783", fontSize: 10, lineHeight: 15, textAlign: "center" }, dangerZone: { padding: 16, borderWidth: 1, borderColor: "#e8b5aa", borderRadius: 14, backgroundColor: "#fff4f1" }, dangerTitle: { color: "#8f3422", fontSize: 9, fontWeight: "900", letterSpacing: .8 }, dangerDescription: { color: "#7f5b53", fontSize: 10, lineHeight: 15, marginTop: 5, marginBottom: 11 }, deleteButton: { minHeight: 46, borderRadius: 11, alignItems: "center", justifyContent: "center", backgroundColor: "#b9472f" }, deleteButtonText: { color: "white", fontSize: 9, fontWeight: "900", letterSpacing: .8 },
  empty: { color: "#829d9a", textAlign: "center", padding: 28 }, error: { color: "#ffc0af", textAlign: "center", fontSize: 12, paddingVertical: 12 },
});
