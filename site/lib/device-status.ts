export type DeviceSystemState = {
  markerType?: string | null;
  markerColor?: string | null;
  location?: [number, number] | null;
  firmwareVersion?: string | null;
  halowMac?: string | null;
  gatewayDeviceId?: string | null;
  gatewayStatus?: string | null;
  wifiEnabled?: boolean | null;
  reportedAt?: string | null;
  lastSeenAt?: string | null;
  topology?: { reportedAt: string | null; links: { peerHalowMac: string; ageMs: number; rssi?: number }[] } | null;
};

export type TopologyLink = {
  $id: string; farmId: string; gatewayDeviceId: string; gatewaySerial: string;
  peerDeviceId: string; peerSerial: string; peerRadioMac: string; rssi?: number | null;
  active: boolean; lastSeenAt: string; reportedAt: string;
};

type DeviceIdentity = DeviceSystemState & { $id: string; serial: string; metadata?: { farmId?: string } };

// Resolve only identities visible through the Devices API. Unknown radios retain their MAC identity.
export function topologyFromDevices(devices: DeviceIdentity[]): TopologyLink[] {
  const byMac = new Map(devices.filter((device) => device.halowMac).map((device) => [device.halowMac!.toUpperCase(), device]));
  return devices.flatMap((device) => {
    const snapshot = device.topology;
    if (!snapshot?.reportedAt || !Number.isFinite(Date.parse(snapshot.reportedAt))) return [];
    const reportedAt = snapshot.reportedAt;
    return snapshot.links.map((link) => {
      const mac = link.peerHalowMac.toUpperCase();
      const peer = byMac.get(mac);
      return {
        $id: `${device.$id}:${mac}`, farmId: device.metadata?.farmId || "",
        gatewayDeviceId: device.$id, gatewaySerial: device.serial,
        peerDeviceId: peer?.$id || `radio:${mac}`, peerSerial: peer?.serial || mac,
        peerRadioMac: mac, rssi: link.rssi, active: true,
        lastSeenAt: new Date(Date.parse(reportedAt) - link.ageMs).toISOString(),
        reportedAt,
      };
    });
  });
}
