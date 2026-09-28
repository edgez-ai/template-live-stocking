import { config, dryRun } from "./appwrite.mjs";

async function request(path, options = {}) {
  const response = await fetch(`${config.endpoint.replace(/\/+$/, "")}${path}`, {
    ...options,
    headers: {
      "content-type": "application/json",
      "x-appwrite-project": config.projectId,
      "x-appwrite-key": config.apiKey,
      ...options.headers,
    },
  });
  if (response.ok) return response.status === 204 ? null : response.json();
  let message = `Appwrite request failed with ${response.status}`;
  try {
    const payload = await response.json();
    if (payload?.message) message = payload.message;
  } catch { /* Keep the status-based error. */ }
  throw Object.assign(new Error(message), { status: response.status });
}

export async function installTimeseries() {
  if (dryRun) {
    console.log(`[dry-run] ensure time-series Store ${config.projectId}`);
    return;
  }

  try {
    await request(`/timeseries/stores/${encodeURIComponent(config.projectId)}`);
    console.log(`Kept existing time-series Store ${config.projectId}`);
    return;
  } catch (caught) {
    // Deployment keys can be write-only, and the native solution deployer can
    // create this Store concurrently. In both cases, POST is the authoritative
    // idempotency check: a conflict means the project-scoped Store now exists.
    if (![401, 403, 404].includes(caught?.status)) throw caught;
  }

  try {
    await request("/timeseries/stores", {
      method: "POST",
      body: JSON.stringify({
        name: config.timeseriesStoreName,
        permissions: config.timeseriesStorePermissions,
      }),
    });
    console.log(`Created time-series Store ${config.projectId}`);
  } catch (caught) {
    if (caught?.status === 409) {
      console.log(`Kept existing time-series Store ${config.projectId}`);
      return;
    }
    if ([401, 403].includes(caught?.status)) {
      throw new Error(
        `${caught.message}. APPWRITE_API_KEY requires the timeseries.write scope`,
        { cause: caught },
      );
    }
    throw caught;
  }
}

export async function removeTimeseries() {
  if (dryRun) {
    console.log(`[dry-run] delete time-series Store ${config.projectId}`);
    return;
  }
  try {
    await request(`/timeseries/stores/${encodeURIComponent(config.projectId)}`, { method: "DELETE" });
    console.log(`Deleted time-series Store ${config.projectId}`);
  } catch (caught) {
    if (caught?.status !== 404) throw caught;
    console.log(`Skipped missing time-series Store ${config.projectId}`);
  }
}
