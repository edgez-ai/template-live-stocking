import AsyncStorage from "@react-native-async-storage/async-storage";
import Constants from "expo-constants";
import * as Location from "expo-location";
import * as TaskManager from "expo-task-manager";
import { Platform } from "react-native";
import { Client, ID, Models, Permission, Query, Role, TablesDB } from "react-native-appwrite";

export type TraceRange = "1h" | "6h" | "24h" | "7d";
export type TracePreferences = { enabled: boolean; intervalMinutes: number; range: TraceRange };
export type TracePoint = {
  id: string;
  userId: string;
  farmId?: string;
  latitude: number;
  longitude: number;
  accuracy?: number;
  altitude?: number;
  speed?: number;
  heading?: number;
  recordedAt: string;
  synced: boolean;
};

type TraceConfig = {
  appwriteEndpoint: string;
  appwriteProjectId: string;
  appwritePlatform: string;
  databaseId: string;
  traceTableId: string;
};
type ActiveTrace = { userId: string; farmId?: string; intervalMinutes: number };
type TraceRow = Models.Row & {
  userId: string;
  farmId?: string | null;
  location: [number, number];
  accuracy?: number | null;
  altitude?: number | null;
  speed?: number | null;
  heading?: number | null;
  recordedAt: string;
};

const TRACE_TASK_NAME = "live-stocking-mobile-trace";
const traceConfig = Constants.expoConfig?.extra as TraceConfig | undefined;
const cachePrefix = `live-stocking:${traceConfig?.appwriteProjectId || "unknown"}:`;
const activeTraceKey = `${cachePrefix}trace:active`;
const tracePointsKey = (userId: string) => `${cachePrefix}trace:points:${userId}`;
const tracePreferencesKey = (userId: string) => `${cachePrefix}trace:preferences:${userId}`;
const defaultPreferences: TracePreferences = { enabled: false, intervalMinutes: 5, range: "24h" };
const maxCachedPoints = 10000;
const maxCacheAgeMs = 30 * 24 * 60 * 60 * 1000;

function traceTables() {
  if (!traceConfig) throw new Error("Trace Appwrite configuration is missing.");
  const endpoint = traceConfig.appwriteEndpoint.replace(/\/+$/, "");
  const client = new Client().setEndpoint(endpoint).setProject(traceConfig.appwriteProjectId).setPlatform(traceConfig.appwritePlatform);
  return new TablesDB(client);
}

async function readActiveTrace() {
  const raw = await AsyncStorage.getItem(activeTraceKey);
  return raw ? JSON.parse(raw) as ActiveTrace : null;
}

export async function loadTracePreferences(userId: string): Promise<TracePreferences> {
  try {
    const raw = await AsyncStorage.getItem(tracePreferencesKey(userId));
    if (!raw) return defaultPreferences;
    const parsed = JSON.parse(raw) as Partial<TracePreferences>;
    const range = parsed.range === "1h" || parsed.range === "6h" || parsed.range === "24h" || parsed.range === "7d" ? parsed.range : defaultPreferences.range;
    const intervalMinutes = [1, 5, 15].includes(Number(parsed.intervalMinutes)) ? Number(parsed.intervalMinutes) : defaultPreferences.intervalMinutes;
    return { enabled: Boolean(parsed.enabled), intervalMinutes, range };
  } catch {
    return defaultPreferences;
  }
}

export async function saveTracePreferences(userId: string, preferences: TracePreferences) {
  await AsyncStorage.setItem(tracePreferencesKey(userId), JSON.stringify(preferences));
}

async function readCachedPoints(userId: string): Promise<TracePoint[]> {
  try {
    const raw = await AsyncStorage.getItem(tracePointsKey(userId));
    const points = raw ? JSON.parse(raw) as TracePoint[] : [];
    return Array.isArray(points) ? points.filter((point) => point.userId === userId && Number.isFinite(point.latitude) && Number.isFinite(point.longitude)) : [];
  } catch {
    return [];
  }
}

async function writeCachedPoints(userId: string, points: TracePoint[]) {
  const cutoff = Date.now() - maxCacheAgeMs;
  const retained = points.filter((point) => Date.parse(point.recordedAt) >= cutoff)
    .sort((a, b) => Date.parse(a.recordedAt) - Date.parse(b.recordedAt))
    .slice(-maxCachedPoints);
  await AsyncStorage.setItem(tracePointsKey(userId), JSON.stringify(retained));
}

function pointFromLocation(location: Location.LocationObject, active: ActiveTrace): TracePoint {
  const finite = (value: number | null) => typeof value === "number" && Number.isFinite(value) ? value : undefined;
  return {
    id: ID.unique(),
    userId: active.userId,
    farmId: active.farmId,
    latitude: location.coords.latitude,
    longitude: location.coords.longitude,
    accuracy: finite(location.coords.accuracy),
    altitude: finite(location.coords.altitude),
    speed: finite(location.coords.speed),
    heading: finite(location.coords.heading),
    recordedAt: new Date(location.timestamp || Date.now()).toISOString(),
    synced: false,
  };
}

async function appendLocations(locations: Location.LocationObject[], active: ActiveTrace) {
  if (!locations.length) return;
  const existing = await readCachedPoints(active.userId);
  const newestTimestamp = existing.length ? Date.parse(existing[existing.length - 1].recordedAt) : 0;
  const additions = locations
    .filter((location) => Number.isFinite(location.coords.latitude) && Number.isFinite(location.coords.longitude) && location.timestamp > newestTimestamp)
    .map((location) => pointFromLocation(location, active));
  if (additions.length) await writeCachedPoints(active.userId, [...existing, ...additions]);
}

export async function syncPendingTracePoints(userId: string) {
  if (!traceConfig) return;
  const points = await readCachedPoints(userId);
  const pending = points.filter((point) => !point.synced);
  if (!pending.length) return;
  const tables = traceTables();
  const syncedIds = new Set<string>();
  for (const point of pending) {
    try {
      await tables.createRow<TraceRow>({
        databaseId: traceConfig.databaseId,
        tableId: traceConfig.traceTableId,
        rowId: point.id,
        data: {
          userId: point.userId,
          farmId: point.farmId || null,
          location: [point.longitude, point.latitude],
          accuracy: point.accuracy ?? null,
          altitude: point.altitude ?? null,
          speed: point.speed ?? null,
          heading: point.heading ?? null,
          recordedAt: point.recordedAt,
        },
        permissions: [Permission.read(Role.user(point.userId)), Permission.update(Role.user(point.userId)), Permission.delete(Role.user(point.userId))],
      });
      syncedIds.add(point.id);
    } catch (caught) {
      if ((caught as { code?: number }).code === 409) syncedIds.add(point.id);
      else break;
    }
  }
  if (syncedIds.size) await writeCachedPoints(userId, points.map((point) => syncedIds.has(point.id) ? { ...point, synced: true } : point));
}

function tracePointFromRow(row: TraceRow): TracePoint | null {
  if (!Array.isArray(row.location) || row.location.length !== 2) return null;
  const [longitude, latitude] = row.location;
  if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) return null;
  return {
    id: row.$id,
    userId: row.userId,
    farmId: row.farmId || undefined,
    latitude,
    longitude,
    accuracy: row.accuracy ?? undefined,
    altitude: row.altitude ?? undefined,
    speed: row.speed ?? undefined,
    heading: row.heading ?? undefined,
    recordedAt: row.recordedAt,
    synced: true,
  };
}

export async function loadTracePoints(userId: string, since: Date): Promise<TracePoint[]> {
  const local = (await readCachedPoints(userId)).filter((point) => Date.parse(point.recordedAt) >= since.getTime());
  let remote: TracePoint[] = [];
  if (traceConfig) {
    try {
      const result = await traceTables().listRows<TraceRow>({
        databaseId: traceConfig.databaseId,
        tableId: traceConfig.traceTableId,
        queries: [Query.equal("userId", userId), Query.greaterThanEqual("recordedAt", since.toISOString()), Query.orderAsc("recordedAt"), Query.limit(5000)],
      });
      remote = result.rows.map(tracePointFromRow).filter((point): point is TracePoint => point !== null);
    } catch {
      // The locally cached trace remains available offline.
    }
  }
  const merged = new Map<string, TracePoint>();
  for (const point of [...remote, ...local]) merged.set(point.id, point);
  const points = [...merged.values()].sort((a, b) => Date.parse(a.recordedAt) - Date.parse(b.recordedAt));
  if (remote.length) await writeCachedPoints(userId, [...await readCachedPoints(userId), ...remote.filter((point) => !local.some((cached) => cached.id === point.id))]);
  return points;
}

export async function enableTraceTracking(userId: string, farmId: string | undefined, preferences: TracePreferences) {
  const foreground = await Location.requestForegroundPermissionsAsync();
  if (foreground.status !== "granted") throw new Error("Location permission is required to record a movement trace.");
  const background = await Location.requestBackgroundPermissionsAsync();
  if (background.status !== "granted") throw new Error("Allow background location so the trace continues when the screen is off.");
  const available = await TaskManager.isAvailableAsync();
  if (!available) throw new Error("Background tracking is unavailable in this app build.");
  const active: ActiveTrace = { userId, farmId, intervalMinutes: preferences.intervalMinutes };
  await AsyncStorage.setItem(activeTraceKey, JSON.stringify(active));
  await saveTracePreferences(userId, { ...preferences, enabled: true });
  try {
    if (await Location.hasStartedLocationUpdatesAsync(TRACE_TASK_NAME)) await Location.stopLocationUpdatesAsync(TRACE_TASK_NAME);
    const intervalMs = preferences.intervalMinutes * 60 * 1000;
    await Location.startLocationUpdatesAsync(TRACE_TASK_NAME, {
      accuracy: Location.Accuracy.Balanced,
      timeInterval: intervalMs,
      distanceInterval: 10,
      deferredUpdatesInterval: intervalMs,
      deferredUpdatesDistance: 10,
      activityType: Location.ActivityType.Fitness,
      pausesUpdatesAutomatically: false,
      showsBackgroundLocationIndicator: true,
      foregroundService: Platform.OS === "android" ? {
        notificationTitle: "Movement trace is active",
        notificationBody: `Saving your position every ${preferences.intervalMinutes} minute${preferences.intervalMinutes === 1 ? "" : "s"}.`,
        notificationColor: "#0A8C87",
      } : undefined,
    });
    const current = await Location.getCurrentPositionAsync({ accuracy: Location.Accuracy.Balanced });
    await appendLocations([current], active);
    await syncPendingTracePoints(userId);
  } catch (caught) {
    await AsyncStorage.removeItem(activeTraceKey);
    await saveTracePreferences(userId, { ...preferences, enabled: false });
    throw caught;
  }
}

export async function disableTraceTracking(userId: string, preferences?: TracePreferences) {
  if (await Location.hasStartedLocationUpdatesAsync(TRACE_TASK_NAME)) await Location.stopLocationUpdatesAsync(TRACE_TASK_NAME);
  await AsyncStorage.removeItem(activeTraceKey);
  const current = preferences ?? await loadTracePreferences(userId);
  await saveTracePreferences(userId, { ...current, enabled: false });
}

if (!TaskManager.isTaskDefined(TRACE_TASK_NAME)) {
  TaskManager.defineTask<{ locations?: Location.LocationObject[] }>(TRACE_TASK_NAME, async ({ data, error }) => {
    if (error || !data?.locations?.length) return;
    const active = await readActiveTrace();
    if (!active) return;
    await appendLocations(data.locations, active);
    await syncPendingTracePoints(active.userId);
  });
}
