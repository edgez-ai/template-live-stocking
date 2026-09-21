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

type Device = { $id: string; serial: string; name: string; status: string; enabled: boolean; metadata?: { farmId?: string }; latitude?: number; longitude?: number };
type Farm = Models.Row & { name: string; country: string; location: string; halowChannel: number; meshId: string; meshPassphrase: string; teamId: string; ownerId: string };
type FarmDetails = { name: string; country: string; location: string; halowChannel: string; meshId: string; meshPassphrase: string };
type Credential = { clientId: string; username: string; password: string };
type Telemetry = Models.Row & { deviceId: string; serial: string; channel: string; topic: string; payload: string; receivedAt: string };
type AppConfig = { appwriteEndpoint: string; appwriteProjectId: string; appwritePlatform: string; databaseId: string; telemetryTableId: string; farmTableId: string };
type HistoryRange = "30m" | "1h" | "6h" | "24h";
type DashboardView = "map" | "list";
type TemperaturePoint = { timestamp: number; value: number };
const emptyFarmDetails: FarmDetails = { name: "", country: "", location: "", halowChannel: "", meshId: "", meshPassphrase: "" };

function detailsFromFarm(farm?: Farm): FarmDetails {
  return farm ? { name: farm.name, country: farm.country, location: farm.location, halowChannel: String(farm.halowChannel), meshId: farm.meshId, meshPassphrase: farm.meshPassphrase } : emptyFarmDetails;
}

function farmData(details: FarmDetails) {
  const country = details.country.trim().toUpperCase();
  const channel = Number(details.halowChannel);
  if (!details.name.trim() || !details.location.trim() || !/^[A-Z]{2}$/.test(country) ||
      !/^\d+$/.test(details.halowChannel) || channel < 1 || channel > 255 ||
      !details.meshId.trim() || details.meshId.trim().length > 32 ||
      details.meshPassphrase.length < 8 || details.meshPassphrase.length > 63) {
    throw new Error("Enter a name, location, two-letter country, channel 1–255, mesh ID, and an 8–63 character passphrase.");
  }
  return { name: details.name.trim(), country, location: details.location.trim(), halowChannel: channel, meshId: details.meshId.trim(), meshPassphrase: details.meshPassphrase };
}

const appConfig = Constants.expoConfig?.extra as AppConfig | undefined;
if (!appConfig) throw new Error("Expo Appwrite configuration is missing");
const config: AppConfig = appConfig;
const endpoint = config.appwriteEndpoint.replace(/\/+$/, "");
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

function messageOf(error: unknown) { return error instanceof Error ? error.message : "Something went wrong."; }

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

function TemperatureChart({ points, duration }: { points: TemperaturePoint[]; duration: number }) {
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
  const padding = Math.max(1, (rawMax - rawMin) * .15);
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
    {!points.length ? <Text style={styles.chartEmpty}>No temperature data in this range.</Text> : null}
    {points.length ? <><Text style={styles.chartMax}>{rawMax.toFixed(1)}°</Text><Text style={styles.chartMin}>{rawMin.toFixed(1)}°</Text></> : null}
  </View>;
}

function FarmFields({ value, onChange }: { value: FarmDetails; onChange: (value: FarmDetails) => void }) {
  const set = (key: keyof FarmDetails, text: string) => onChange({ ...value, [key]: text });
  return <>
    <Text style={styles.fieldLabel}>FARM NAME</Text>
    <TextInput style={styles.inputLight} value={value.name} onChangeText={(text) => set("name", text)} placeholder="Farm name" maxLength={128} />
    <Text style={styles.fieldLabel}>COUNTRY CODE</Text>
    <TextInput style={styles.inputLight} value={value.country} onChangeText={(text) => set("country", text.toUpperCase())} placeholder="SE" autoCapitalize="characters" maxLength={2} />
    <Text style={styles.fieldLabel}>LOCATION</Text>
    <TextInput style={styles.inputLight} value={value.location} onChangeText={(text) => set("location", text)} placeholder="Farm location" maxLength={128} />
    <Text style={styles.fieldLabel}>HALOW CHANNEL</Text>
    <TextInput style={styles.inputLight} value={value.halowChannel} onChangeText={(text) => set("halowChannel", text)} placeholder="Channel number" keyboardType="number-pad" maxLength={3} />
    <Text style={styles.fieldLabel}>MESH ID</Text>
    <TextInput style={styles.inputLight} value={value.meshId} onChangeText={(text) => set("meshId", text)} placeholder="Mesh ID" maxLength={32} autoCapitalize="none" />
    <Text style={styles.fieldLabel}>MESH PASSPHRASE</Text>
    <TextInput style={styles.inputLight} value={value.meshPassphrase} onChangeText={(text) => set("meshPassphrase", text)} placeholder="At least 8 characters" secureTextEntry maxLength={63} autoCapitalize="none" />
  </>;
}

function OfflineMap({ devices }: { devices: Device[] }) {
  const map = useRef<EdgezOrganicMapRef>(null);
  const [region, setRegion] = useState("");
  const [download, setDownload] = useState<EdgezMapDownloadUpdate | null>(null);
  const [mapError, setMapError] = useState("");
  const markers = useMemo<EdgezMapNode[]>(() => devices.flatMap((device) =>
    Number.isFinite(device.latitude) && Number.isFinite(device.longitude) ? [{
      id: device.$id,
      label: device.name,
      latitude: device.latitude!,
      longitude: device.longitude!,
      marker: device.enabled ? "blue" : "gray",
    }] : [],
  ), [devices]);

  return <View style={styles.mapCard}>
    <EdgezOrganicMap
      ref={map}
      nodes={markers}
      centerLatitude={59.3293}
      centerLongitude={18.0686}
      zoom={9}
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

async function deviceApi<T>(path = "", method: "GET" | "POST" | "DELETE" = "GET", body?: object) {
  const response = await fetch(`${endpoint}/devices${path}`, {
    method,
    credentials: "include",
    headers: { "content-type": "application/json", "x-appwrite-project": config.appwriteProjectId },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (response.status === 204) return undefined as T;
  const payload = await response.json() as T & { message?: string };
  if (!response.ok) throw new Error(payload.message || "Appwrite Devices request failed.");
  return payload;
}

export default function App() {
  const [user, setUser] = useState<Models.User<Models.Preferences> | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [farms, setFarms] = useState<Farm[]>([]);
  const [currentFarmId, setCurrentFarmId] = useState("");
  const [settingsOpen, setSettingsOpen] = useState(false);
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
  const [useUpstreamWifi, setUseUpstreamWifi] = useState<boolean | null>(null);
  const [upstreamSsid, setUpstreamSsid] = useState("");
  const [upstreamPassword, setUpstreamPassword] = useState("");
  const [upstreamNetworks, setUpstreamNetworks] = useState<ESPWifiList[]>([]);
  const [provisioningStatus, setProvisioningStatus] = useState("");
  const [provisioningDialogOpen, setProvisioningDialogOpen] = useState(false);
  const [provisioningStep, setProvisioningStep] = useState<1 | 2 | 3 | 4>(1);
  const [detailDevice, setDetailDevice] = useState<Device | null>(null);
  const [historyRange, setHistoryRange] = useState<HistoryRange>("1h");
  const [historyPoints, setHistoryPoints] = useState<TemperaturePoint[]>([]);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState("");
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState("");
  const [dashboardView, setDashboardView] = useState<DashboardView>("map");
  const [menuOpen, setMenuOpen] = useState(false);

  const refresh = useCallback(async () => {
    const [farmResult, deviceResult, telemetryResult] = await Promise.all([
      tables.listRows<Farm>({ databaseId: config.databaseId, tableId: config.farmTableId, queries: [Query.limit(100)] }),
      deviceApi<{ devices: Device[] }>(),
      tables.listRows({ databaseId: config!.databaseId, tableId: config!.telemetryTableId, queries: [Query.orderDesc("receivedAt"), Query.limit(500)] }),
    ]);
    setFarms(farmResult.rows);
    setCurrentFarmId((selected) => farmResult.rows.some((farm) => farm.$id === selected) ? selected : farmResult.rows[0]?.$id || "");
    setDevices(deviceResult.devices);
    setTelemetry(telemetryResult.rows as unknown as Telemetry[]);
    return farmResult.rows;
  }, []);

  useEffect(() => {
    account.get().then(async (current) => {
      setUser(current);
      setCurrentFarmId((current.prefs as { currentFarmId?: string }).currentFarmId || "");
      try {
        const availableFarms = await refresh();
        if (!availableFarms.length) setSettingsOpen(true);
      } catch (caught) { setError(messageOf(caught)); }
    }).catch(() => undefined).finally(() => setBusy(false));
  }, [refresh]);
  useEffect(() => {
    if (!user) return;
    const timer = setInterval(() => {
      void refresh().catch((caught) => setError(messageOf(caught)));
    }, 5000);
    return () => clearInterval(timer);
  }, [refresh, user]);
  useEffect(() => {
    if (!detailDevice) return;
    let active = true;
    const duration = historyRanges.find((range) => range.key === historyRange)!.duration;
    setHistoryLoading(true); setHistoryError(""); setHistoryPoints([]);
    tables.listRows({
      databaseId: config.databaseId,
      tableId: config.telemetryTableId,
      queries: [Query.equal("deviceId", detailDevice.$id), Query.greaterThanEqual("receivedAt", new Date(Date.now() - duration).toISOString()), Query.orderAsc("receivedAt"), Query.limit(5000)],
    }).then((result) => {
      if (!active) return;
      const points = (result.rows as unknown as Telemetry[]).flatMap((row) => {
        const value = temperatureOf(row);
        return value === null ? [] : [{ timestamp: new Date(row.receivedAt).getTime(), value }];
      });
      setHistoryPoints(points);
    }).catch((caught) => { if (active) { setHistoryPoints([]); setHistoryError(messageOf(caught)); } })
      .finally(() => { if (active) setHistoryLoading(false); });
    return () => { active = false; };
  }, [detailDevice, historyRange]);

  async function authenticate(register: boolean) {
    setBusy(true); setError("");
    try {
      if (register) await account.create({ userId: ID.unique(), email, password });
      await account.createEmailPasswordSession({ email, password });
      const current = await account.get();
      setUser(current);
      setCurrentFarmId((current.prefs as { currentFarmId?: string }).currentFarmId || "");
      const availableFarms = await refresh();
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
    if (!currentFarmId) { openSettings(); return; }
    setMenuOpen(false);
    selectedBleDevice?.disconnect();
    setBleDevices([]); setSelectedBleDevice(null); setProofOfPossession(provisioningPop); setBleConnected(false); setName(""); setUseUpstreamWifi(null); setUpstreamSsid(""); setUpstreamPassword(""); setUpstreamNetworks([]);
    setError(""); setProvisioningStatus("Start by scanning for a device in provisioning mode.");
    setProvisioningStep(1);
    setProvisioningDialogOpen(true);
  }

  function closeProvisioningDialog() {
    if (busy) return;
    selectedBleDevice?.disconnect();
    setSelectedBleDevice(null); setBleConnected(false);
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
    setProofOfPossession(provisioningPop);
    setBleConnected(false);
    setProvisioningStatus(`Confirm the details for ${device.name}.`);
    setProvisioningStep(2);
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
    if (!user || !farm || !selectedBleDevice || !bleConnected) return;
    setBusy(true); setError(""); setProvisioningStatus("Preparing the device credential…");
    try {
      const serial = serialFromBleName(selectedBleDevice.name);

      setProvisioningStatus("Creating Appwrite device credential…");
      let appwriteDevice = devices.find((device) => device.serial === serial);
      if (appwriteDevice && appwriteDevice.metadata?.farmId !== farm.$id) {
        throw new Error("This device is already assigned to another farm. Open Settings to select that farm.");
      }
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

      setProvisioningStatus("Sending MQTT credential to the device…");
      const mqttResponse = await selectedBleDevice.sendData("mqtt-config", JSON.stringify({
        clientId: mqtt.clientId,
        username: mqtt.username,
        password: mqtt.password,
        projectId: config.appwriteProjectId,
        channel: "status",
      }));
      const accepted = JSON.parse(mqttResponse) as { ok?: boolean };
      if (!accepted.ok) throw new Error("The device rejected its MQTT credential.");

      setProvisioningStatus(`Sending ${farm.name} mesh configuration…`);
      const halowResponse = await selectedBleDevice.sendData("halow-config", JSON.stringify({
        meshId: farm.meshId,
        passphrase: farm.meshPassphrase,
        country: farm.country,
        channel: farm.halowChannel,
        wifiUpstream: useUpstreamWifi === true,
      }));
      const halowAccepted = JSON.parse(halowResponse) as { ok?: boolean; error?: string };
      if (!halowAccepted.ok) throw new Error(halowAccepted.error || "The device rejected the HaLow credentials.");
      if (useUpstreamWifi) {
        setProvisioningStatus("Connecting the ESP32 to upstream Wi-Fi…");
        await selectedBleDevice.provision(upstreamSsid, upstreamPassword);
      }
      setProvisioningStatus(`Provisioned ${serial}. Waiting for temperature telemetry.`);
      setSelectedBleDevice(null); setBleDevices([]); setProofOfPossession(provisioningPop); setBleConnected(false); setName("");
      setProvisioningDialogOpen(false);
      await refresh();
    } catch (caught) { setError(messageOf(caught)); setProvisioningStatus("Provisioning did not complete."); }
    finally { selectedBleDevice.disconnect(); setBleConnected(false); setBusy(false); }
  }

  async function signOut() {
    setMenuOpen(false);
    selectedBleDevice?.disconnect();
    await account.deleteSession({ sessionId: "current" });
    setUser(null); setDevices([]); setFarms([]); setCurrentFarmId(""); setSettingsOpen(false); setTelemetry([]); setBleDevices([]); setSelectedBleDevice(null); setProofOfPossession(provisioningPop); setBleConnected(false); setProvisioningDialogOpen(false); setDetailDevice(null); setDashboardView("map");
  }

  function openSettings() {
    setMenuOpen(false);
    setFarmDraft(detailsFromFarm(farms.find((farm) => farm.$id === currentFarmId)));
    setSettingsOpen(true);
  }

  async function selectFarm(farm: Farm) {
    if (!user) return;
    setBusy(true); setError("");
    try {
      const updated = await account.updatePrefs({ prefs: { ...user.prefs, currentFarmId: farm.$id } });
      setUser(updated);
      setCurrentFarmId(farm.$id);
      setFarmDraft(detailsFromFarm(farm));
      setDetailDevice(null);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function createFarm() {
    if (!user) return;
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
      setFarms((existing) => [...existing, farm]);
      setNewFarm(emptyFarmDetails);
      await selectFarm(farm);
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
  }

  async function saveFarm() {
    const farm = farms.find((candidate) => candidate.$id === currentFarmId);
    if (!farm || farm.ownerId !== user?.$id) return;
    setBusy(true); setError("");
    try {
      const data = farmData(farmDraft);
      const updated = await tables.updateRow<Farm>({ databaseId: config.databaseId, tableId: config.farmTableId, rowId: farm.$id, data });
      if (data.name !== farm.name) await teams.updateName({ teamId: farm.teamId, name: data.name });
      setFarms((existing) => existing.map((item) => item.$id === farm.$id ? updated : item));
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
    setBusy(true); setError("");
    try {
      await deviceApi<void>(`/${encodeURIComponent(device.$id)}`, "DELETE");
      setDetailDevice(null);
      await refresh();
    } catch (caught) { setError(messageOf(caught)); }
    finally { setBusy(false); }
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
  const historyStats = useMemo(() => {
    if (!historyPoints.length) return null;
    const values = historyPoints.map((point) => point.value);
    return { min: Math.min(...values), max: Math.max(...values), average: values.reduce((sum, value) => sum + value, 0) / values.length };
  }, [historyPoints]);
  const activeHistoryRange = historyRanges.find((range) => range.key === historyRange)!;
  const activeHistoryDuration = activeHistoryRange.duration;
  const detailLatest = detailDevice ? latestTemperatureByDevice.get(detailDevice.$id) : undefined;
  const detailStatus = detailDevice ? statusOf(detailDevice, detailLatest?.row) : "";
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
      {dashboardView === "map" ? <OfflineMap devices={visibleDevices} /> : <ScrollView style={styles.listScroll} contentContainerStyle={styles.listContent} keyboardShouldPersistTaps="handled">
      <Text style={styles.sectionLabel}>{visibleDevices.length} DEVICES</Text>
      {visibleDevices.map((device) => {
        const latest = latestTemperatureByDevice.get(device.$id);
        const status = statusOf(device, latest?.row);
        return <Pressable key={device.$id} style={styles.deviceCard} onPress={() => { setHistoryRange("1h"); setDetailDevice(device); }}>
          <View style={styles.deviceCardHeader}><View><Text style={styles.deviceCardName}>{device.name}</Text><Text style={styles.deviceSerial}>{device.serial}</Text></View><View style={styles.statusBadge}><View style={[styles.statusDot, status === "Online" ? styles.statusOnline : styles.statusOffline]} /><Text style={styles.statusText}>{status.toUpperCase()}</Text></View></View>
          <View style={styles.latestRow}><View><Text style={styles.latestLabel}>LATEST INTERNAL TEMPERATURE</Text><Text style={styles.latestValue}>{latest ? `${latest.value.toFixed(1)}°C` : "—"}</Text></View><Text style={styles.cardArrow}>›</Text></View>
          <Text style={styles.lastSeen}>{latest ? `Updated ${relativeTime(latest.row.receivedAt)}` : "Waiting for temperature telemetry"}</Text>
        </Pressable>;
      })}
      {!currentFarm ? <Text style={styles.empty}>Create a farm in Settings before adding devices.</Text> : !visibleDevices.length ? <Text style={styles.empty}>No devices in this farm yet. Use + ADD to provision one.</Text> : null}
      </ScrollView>}
      {menuOpen && <Pressable style={styles.menuBackdrop} onPress={() => setMenuOpen(false)} accessibilityLabel="Close menu" />}
      <View style={[styles.dashboardHeader, dashboardView === "list" && styles.listHeader]}>
        {dashboardView === "list" ? <Text style={styles.listTitle} numberOfLines={1}>{currentFarm?.name || "Choose a farm"}</Text> : <View />}
        <View style={styles.headerActions}>
          <Pressable style={styles.addButton} onPress={openProvisioningDialog} disabled={busy} accessibilityRole="button" accessibilityLabel="Add device"><Text style={styles.addButtonText}>+ ADD</Text></Pressable>
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
    <Modal visible={settingsOpen && Boolean(user)} animationType="slide" onRequestClose={() => setSettingsOpen(false)}>
      <SafeAreaView style={styles.dialogPage}>
        <View style={styles.dialogHeader}><View><Text style={styles.stepLabel}>SETTINGS</Text><Text style={styles.dialogTitle}>Farms</Text></View><Pressable onPress={() => setSettingsOpen(false)}><Text style={styles.close}>CLOSE</Text></Pressable></View>
        <ScrollView contentContainerStyle={styles.dialogContent} keyboardShouldPersistTaps="handled">
          <Text style={styles.dialogHelp}>Choose the farm for this app. The selected farm is saved to your account and opens next time.</Text>
          {farms.map((farm) => <Pressable key={farm.$id} style={[styles.farmRow, farm.$id === currentFarmId && styles.farmRowSelected]} onPress={() => void selectFarm(farm)} disabled={busy} accessibilityRole="button" accessibilityState={{ selected: farm.$id === currentFarmId }}><Text style={styles.deviceNameDark}>{farm.name}</Text><Text style={styles.muted}>{farm.$id === currentFarmId ? "CURRENT FARM" : "TAP TO SWITCH"}</Text></Pressable>)}
          {!farms.length && <Text style={styles.muted}>No farms yet. Create the first farm below.</Text>}
          <Text style={styles.fieldLabel}>CREATE FARM</Text>
          <FarmFields value={newFarm} onChange={setNewFarm} />
          <Pressable style={styles.primary} onPress={() => void createFarm()} disabled={busy}><Text style={styles.primaryText}>CREATE FARM & TEAM</Text></Pressable>
          {currentFarm && currentFarm.ownerId === user?.$id && <>
            <Text style={styles.fieldLabel}>EDIT CURRENT FARM</Text>
            <FarmFields value={farmDraft} onChange={setFarmDraft} />
            <Pressable style={styles.primary} onPress={() => void saveFarm()} disabled={busy}><Text style={styles.primaryText}>SAVE FARM</Text></Pressable>
          </>}
          {error ? <Text style={styles.dialogError}>{error}</Text> : null}
        </ScrollView>
      </SafeAreaView>
    </Modal>
    <Modal visible={provisioningDialogOpen} animationType="slide" onRequestClose={closeProvisioningDialog}>
      <SafeAreaView style={styles.dialogPage}>
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
              <View style={styles.selectedSummary}><Text style={styles.deviceNameDark}>{currentFarm?.name}</Text><Text style={styles.muted}>{currentFarm?.location}, {currentFarm?.country} · Channel {currentFarm?.halowChannel}</Text><Text style={styles.muted}>Mesh ID: {currentFarm?.meshId}</Text></View>
              <Text style={styles.dialogHelp}>{useUpstreamWifi ? `Upstream Wi-Fi: ${upstreamSsid}. ` : "No upstream Wi-Fi. "}The app will send the mesh settings and MQTT credential to the device.</Text>
              <Pressable style={styles.primary} onPress={provisionDevice} disabled={busy || !bleConnected || !currentFarm}><Text style={styles.primaryText}>{busy ? "PROVISIONING…" : "PROVISION DEVICE"}</Text></Pressable>
            </>}
            {provisioningStep > 1 && <Pressable style={styles.backButton} onPress={previousProvisioningStep} disabled={busy}><Text style={styles.secondaryText}>BACK</Text></Pressable>}
            {provisioningStatus ? <Text style={styles.dialogStatus}>{provisioningStatus}</Text> : null}
            {error ? <Text style={styles.dialogError}>{error}</Text> : null}
          </ScrollView>
        </KeyboardAvoidingView>
      </SafeAreaView>
    </Modal>
    <Modal visible={Boolean(detailDevice)} animationType="slide" onRequestClose={() => setDetailDevice(null)}>
      <SafeAreaView style={styles.detailPage}>
        {detailDevice ? <>
          <View style={styles.detailHeader}><Pressable onPress={() => setDetailDevice(null)}><Text style={styles.detailBack}>‹ DEVICES</Text></Pressable><Text style={styles.detailSerial}>{detailDevice.serial}</Text></View>
          <ScrollView contentContainerStyle={styles.detailContent}>
            <View style={styles.detailTitleRow}><View><Text style={styles.detailEyebrow}>DEVICE</Text><Text style={styles.detailTitle}>{detailDevice.name}</Text></View><View style={styles.detailStatus}><View style={[styles.statusDot, detailStatus === "Online" ? styles.statusOnline : styles.statusOffline]} /><Text style={styles.detailStatusText}>{detailStatus.toUpperCase()}</Text></View></View>
            <View style={styles.detailLatest}><Text style={styles.detailMetricLabel}>INTERNAL TEMPERATURE</Text><Text style={styles.detailMetricValue}>{detailLatest ? `${detailLatest.value.toFixed(1)}°C` : "—"}</Text><Text style={styles.detailMetricTime}>{detailLatest ? `Updated ${relativeTime(detailLatest.row.receivedAt)}` : "No readings received"}</Text></View>
            <Text style={styles.rangeTitle}>HISTORY RANGE</Text>
            <View style={styles.rangeSelector}>{historyRanges.map((range) => <Pressable key={range.key} style={[styles.rangeButton, historyRange === range.key && styles.rangeButtonActive]} onPress={() => setHistoryRange(range.key)} disabled={historyLoading}><Text style={[styles.rangeButtonText, historyRange === range.key && styles.rangeButtonTextActive]}>{range.label}</Text></Pressable>)}</View>
            <View style={styles.chartCard}><View style={styles.chartCardHeader}><View><Text style={styles.chartTitle}>Temperature</Text><Text style={styles.chartSubtitle}>ESP32-S3 internal sensor · {historyPoints.length} readings</Text></View>{historyLoading ? <ActivityIndicator color="#0a8c87" /> : null}</View><TemperatureChart points={historyPoints} duration={activeHistoryDuration} /><View style={styles.chartAxis}><Text style={styles.chartAxisText}>{activeHistoryRange.label} AGO</Text><Text style={styles.chartAxisText}>NOW</Text></View>{historyError ? <Text style={styles.historyError}>{historyError}</Text> : null}</View>
            {historyStats ? <View style={styles.statsRow}><View style={styles.stat}><Text style={styles.statLabel}>MIN</Text><Text style={styles.statValue}>{historyStats.min.toFixed(1)}°</Text></View><View style={styles.stat}><Text style={styles.statLabel}>AVERAGE</Text><Text style={styles.statValue}>{historyStats.average.toFixed(1)}°</Text></View><View style={styles.stat}><Text style={styles.statLabel}>MAX</Text><Text style={styles.statValue}>{historyStats.max.toFixed(1)}°</Text></View></View> : null}
            <Text style={styles.sensorNote}>This is the ESP32-S3 chip temperature, not ambient room temperature.</Text>
            <View style={styles.dangerZone}><Text style={styles.dangerTitle}>DEVICE ACCESS</Text><Text style={styles.dangerDescription}>Deleting this device revokes its MQTT credential. Historical telemetry is retained.</Text><Pressable style={styles.deleteButton} onPress={() => requestDeleteDevice(detailDevice)} disabled={busy}><Text style={styles.deleteButtonText}>{busy ? "DELETING…" : "DELETE DEVICE"}</Text></Pressable></View>
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
  auth: { flex: 1, justifyContent: "center", gap: 8, paddingBottom: 40 }, hero: { color: "white", fontSize: 45, lineHeight: 51, fontWeight: "900", letterSpacing: -1.8, marginBottom: 24 }, accent: { color: "#ff8264" }, authLabel: { color: "#90aaa7", fontSize: 9, fontWeight: "900", letterSpacing: 1, marginTop: 5 },
  input: { height: 58, borderWidth: 1, borderColor: "#36565b", borderRadius: 14, paddingHorizontal: 17, color: "white", fontSize: 16 }, primary: { minHeight: 54, borderRadius: 14, alignItems: "center", justifyContent: "center", backgroundColor: "#0a8c87", marginTop: 5 }, primaryText: { color: "white", fontSize: 10, fontWeight: "900", letterSpacing: .8 }, secondary: { minHeight: 52, alignItems: "center", justifyContent: "center" }, secondaryText: { color: "#69cfc7", fontSize: 11, fontWeight: "900", letterSpacing: 1 },
  listScroll: { flex: 1 }, listContent: { paddingHorizontal: 22, paddingTop: 94, paddingBottom: 30, gap: 12 }, inputLight: { height: 54, borderWidth: 1, borderColor: "#cedbdc", borderRadius: 12, paddingHorizontal: 15, color: "#0a3037", backgroundColor: "white" }, muted: { color: "#59716f", fontSize: 11 },
  mapCard: { flex: 1, overflow: "hidden", backgroundColor: "#dce8e5" }, map: { ...StyleSheet.absoluteFill }, mapPrompt: { position: "absolute", left: 12, right: 12, bottom: 28, padding: 14, borderRadius: 14, backgroundColor: "#092e35f2" }, mapPromptTitle: { color: "white", fontSize: 14, fontWeight: "900" }, mapPromptText: { color: "#b8cdca", fontSize: 11, lineHeight: 16, marginTop: 3 }, mapPromptActions: { flexDirection: "row", gap: 8, marginTop: 10 }, mapDownloadButton: { minHeight: 38, justifyContent: "center", paddingHorizontal: 13, borderRadius: 9, backgroundColor: "#0a8c87" }, mapDownloadText: { color: "white", fontSize: 9, fontWeight: "900", letterSpacing: .7 }, mapLaterButton: { minHeight: 38, justifyContent: "center", paddingHorizontal: 13 }, mapLaterText: { color: "#90aaa7", fontSize: 9, fontWeight: "900", letterSpacing: .7 }, mapNotice: { position: "absolute", left: 12, right: 90, top: 68, borderRadius: 9, padding: 9, backgroundColor: "#092e35e8" }, mapError: { backgroundColor: "#8f3422e8" }, mapNoticeText: { color: "white", fontSize: 10, fontWeight: "700" }, mapAttribution: { position: "absolute", left: 10, right: 10, bottom: 5, color: "#173e43", fontSize: 9, textShadowColor: "white", textShadowRadius: 4 },
  farmRow: { padding: 16, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white", gap: 4 }, farmRowSelected: { borderColor: "#0a8c87", borderWidth: 2, backgroundColor: "#e9f7f5" },
  bleDevice: { padding: 13, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white" }, deviceNameDark: { color: "#0a3037", fontSize: 14, fontWeight: "800" },
  wifiHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", marginTop: 4 }, rescan: { color: "#0a8c87", fontSize: 10, fontWeight: "900" }, wifiNetwork: { padding: 12, borderRadius: 12, borderWidth: 1, borderColor: "#cedbdc", backgroundColor: "white", flexDirection: "row", justifyContent: "space-between", alignItems: "center" }, wifiNetworkSelected: { borderColor: "#0a8c87", borderWidth: 2, backgroundColor: "#e9f7f5" }, signal: { color: "#59716f", fontSize: 10, fontWeight: "700" },
  dialogPage: { flex: 1, backgroundColor: "#f7faf9" }, dialogScreen: { flex: 1 }, dialogHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", paddingHorizontal: 22, paddingVertical: 18, borderBottomWidth: 1, borderBottomColor: "#dce6e5" }, stepLabel: { color: "#0a8c87", fontSize: 9, fontWeight: "900", letterSpacing: 1 }, dialogTitle: { color: "#0a3037", fontSize: 24, fontWeight: "900", marginTop: 3 }, close: { color: "#59716f", fontSize: 10, fontWeight: "900" }, dialogContent: { padding: 22, paddingBottom: 34, gap: 10 }, dialogHelp: { color: "#59716f", fontSize: 13, lineHeight: 19 }, selectedSummary: { padding: 13, borderRadius: 12, backgroundColor: "#e9f7f5", marginBottom: 4 }, fieldLabel: { color: "#385753", fontSize: 9, fontWeight: "900", letterSpacing: .9, marginTop: 5 }, fieldHint: { color: "#718783", fontSize: 10, marginTop: -5 }, backButton: { minHeight: 44, alignItems: "center", justifyContent: "center" }, dialogStatus: { color: "#59716f", fontSize: 11, textAlign: "center", marginTop: 2 }, dialogError: { color: "#b9472f", fontSize: 11, textAlign: "center" },
  sectionLabel: { color: "#7d9a97", fontSize: 10, fontWeight: "900", letterSpacing: 1.2, marginTop: 6 }, deviceCard: { padding: 18, borderRadius: 18, backgroundColor: "#f7faf9" }, deviceCardHeader: { flexDirection: "row", alignItems: "flex-start", justifyContent: "space-between" }, deviceCardName: { color: "#0a3037", fontSize: 18, fontWeight: "900" }, deviceSerial: { color: "#718783", fontSize: 10, fontWeight: "700", letterSpacing: .7, marginTop: 3 }, statusBadge: { flexDirection: "row", alignItems: "center", gap: 6, paddingHorizontal: 9, paddingVertical: 6, borderRadius: 20, backgroundColor: "#e7efed" }, statusDot: { width: 7, height: 7, borderRadius: 4 }, statusOnline: { backgroundColor: "#16a085" }, statusOffline: { backgroundColor: "#9badaa" }, statusText: { color: "#4d6965", fontSize: 8, fontWeight: "900", letterSpacing: .7 }, latestRow: { flexDirection: "row", alignItems: "flex-end", justifyContent: "space-between", marginTop: 24 }, latestLabel: { color: "#718783", fontSize: 8, fontWeight: "900", letterSpacing: .8 }, latestValue: { color: "#0a3037", fontSize: 39, lineHeight: 45, fontWeight: "900", letterSpacing: -1.5 }, cardArrow: { color: "#0a8c87", fontSize: 36, lineHeight: 42, fontWeight: "300" }, lastSeen: { color: "#718783", fontSize: 10, marginTop: 5 },
  detailPage: { flex: 1, backgroundColor: "#eef4f2" }, detailHeader: { minHeight: 58, paddingHorizontal: 20, flexDirection: "row", alignItems: "center", justifyContent: "space-between", borderBottomWidth: 1, borderBottomColor: "#d7e3e0" }, detailBack: { color: "#0a8c87", fontSize: 10, fontWeight: "900", letterSpacing: .8 }, detailSerial: { color: "#718783", fontSize: 9, fontWeight: "800", letterSpacing: .7 }, detailContent: { padding: 22, paddingBottom: 40, gap: 14 }, detailTitleRow: { flexDirection: "row", alignItems: "center", justifyContent: "space-between" }, detailEyebrow: { color: "#0a8c87", fontSize: 9, fontWeight: "900", letterSpacing: 1 }, detailTitle: { color: "#0a3037", fontSize: 30, fontWeight: "900", marginTop: 2 }, detailStatus: { flexDirection: "row", alignItems: "center", gap: 6 }, detailStatusText: { color: "#4d6965", fontSize: 9, fontWeight: "900", letterSpacing: .8 }, detailLatest: { padding: 19, borderRadius: 18, backgroundColor: "#0a3037" }, detailMetricLabel: { color: "#69cfc7", fontSize: 9, fontWeight: "900", letterSpacing: .9 }, detailMetricValue: { color: "white", fontSize: 45, lineHeight: 54, fontWeight: "900", letterSpacing: -1.5 }, detailMetricTime: { color: "#90aaa7", fontSize: 10 }, rangeTitle: { color: "#385753", fontSize: 9, fontWeight: "900", letterSpacing: .9, marginTop: 5 }, rangeSelector: { flexDirection: "row", padding: 4, borderRadius: 12, backgroundColor: "#dce8e5" }, rangeButton: { flex: 1, minHeight: 38, borderRadius: 9, alignItems: "center", justifyContent: "center" }, rangeButtonActive: { backgroundColor: "#0a8c87" }, rangeButtonText: { color: "#59716f", fontSize: 8, fontWeight: "900" }, rangeButtonTextActive: { color: "white" }, chartCard: { padding: 16, borderRadius: 18, backgroundColor: "white" }, chartCardHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", marginBottom: 8 }, chartTitle: { color: "#0a3037", fontSize: 17, fontWeight: "900" }, chartSubtitle: { color: "#718783", fontSize: 9, marginTop: 2 }, chart: { height: 210, overflow: "hidden", borderRadius: 12, backgroundColor: "#f3f8f6" }, chartGrid: { position: "absolute", left: 18, right: 18, height: 1, backgroundColor: "#dce8e5" }, chartLine: { position: "absolute", height: 2, borderRadius: 1, backgroundColor: "#0a8c87" }, chartDot: { position: "absolute", width: 8, height: 8, borderRadius: 4, backgroundColor: "#ff8264", borderWidth: 2, borderColor: "white" }, chartEmpty: { color: "#718783", fontSize: 11, textAlign: "center", marginTop: 95 }, chartMax: { position: "absolute", top: 4, right: 6, color: "#718783", fontSize: 8, fontWeight: "800" }, chartMin: { position: "absolute", bottom: 4, right: 6, color: "#718783", fontSize: 8, fontWeight: "800" }, chartAxis: { flexDirection: "row", justifyContent: "space-between", marginTop: 6 }, chartAxisText: { color: "#718783", fontSize: 8, fontWeight: "800" }, historyError: { color: "#b9472f", fontSize: 10, textAlign: "center", marginTop: 8 }, statsRow: { flexDirection: "row", gap: 10 }, stat: { flex: 1, padding: 13, borderRadius: 14, backgroundColor: "white" }, statLabel: { color: "#718783", fontSize: 8, fontWeight: "900", letterSpacing: .7 }, statValue: { color: "#0a3037", fontSize: 19, fontWeight: "900", marginTop: 4 }, sensorNote: { color: "#718783", fontSize: 10, lineHeight: 15, textAlign: "center" }, dangerZone: { padding: 16, borderWidth: 1, borderColor: "#e8b5aa", borderRadius: 14, backgroundColor: "#fff4f1" }, dangerTitle: { color: "#8f3422", fontSize: 9, fontWeight: "900", letterSpacing: .8 }, dangerDescription: { color: "#7f5b53", fontSize: 10, lineHeight: 15, marginTop: 5, marginBottom: 11 }, deleteButton: { minHeight: 46, borderRadius: 11, alignItems: "center", justifyContent: "center", backgroundColor: "#b9472f" }, deleteButtonText: { color: "white", fontSize: 9, fontWeight: "900", letterSpacing: .8 },
  empty: { color: "#829d9a", textAlign: "center", padding: 28 }, error: { color: "#ffc0af", textAlign: "center", fontSize: 12, paddingVertical: 12 },
});
