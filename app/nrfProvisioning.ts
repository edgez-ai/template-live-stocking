import { fromByteArray, toByteArray } from "base64-js";
import { BleManager, type Device } from "react-native-ble-plx";

const serviceUuid = "a3631000-b82e-44c2-9b1d-a790675b4ac1";
const configUuid = "a3631001-b82e-44c2-9b1d-a790675b4ac1";
const statusUuid = "a3631002-b82e-44c2-9b1d-a790675b4ac1";
let manager: BleManager | null = null;
function bleManager(): BleManager {
  manager ??= new BleManager();
  return manager;
}

function utf8Bytes(value: string): Uint8Array {
  const encoded = encodeURIComponent(value);
  const bytes: number[] = [];
  for (let i = 0; i < encoded.length; i++) {
    if (encoded[i] === "%") {
      bytes.push(parseInt(encoded.slice(i + 1, i + 3), 16));
      i += 2;
    } else bytes.push(encoded.charCodeAt(i));
  }
  return Uint8Array.from(bytes);
}

export class NrfProvisioningDevice {
  readonly kind = "nrf" as const;
  private connected: Device | null = null;

  constructor(readonly id: string, readonly name: string) {}

  async connect(): Promise<void> {
    this.connected = await bleManager().connectToDevice(this.id, { autoConnect: false });
    await this.connected.discoverAllServicesAndCharacteristics();
    try { this.connected = await this.connected.requestMTU(247); } catch { /* Use the default ATT MTU. */ }
  }

  async sendMqttConfig(json: string): Promise<string> {
    const device = this.connected;
    if (!device) throw new Error("The nRF54 is not connected.");
    const payload = utf8Bytes(json);
    if (!payload.length || payload.length > 1024) throw new Error("Device configuration exceeds the BLE limit.");
    const chunkSize = Math.max(1, Math.min(160, (device.mtu || 23) - 6));
    for (let offset = 0; offset < payload.length;) {
      const first = offset === 0;
      const count = Math.min(payload.length - offset, chunkSize);
      const header = first ? [1, payload.length & 255, payload.length >> 8] : [2];
      const chunk = Uint8Array.from([...header, ...payload.slice(offset, offset + count)]);
      await device.writeCharacteristicWithResponseForService(serviceUuid, configUuid, fromByteArray(chunk));
      offset += count;
    }
    const status = await device.readCharacteristicForService(serviceUuid, statusUuid);
    if (!status.value) throw new Error("The nRF54 returned no configuration status.");
    return String.fromCharCode(...toByteArray(status.value));
  }

  disconnect(): void {
    if (this.connected) void bleManager().cancelDeviceConnection(this.id).catch(() => undefined);
    this.connected = null;
  }
}

export async function scanNrfProvisioningDevices(): Promise<NrfProvisioningDevice[]> {
  const found = new Map<string, NrfProvisioningDevice>();
  const client = bleManager();
  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => { client.stopDeviceScan(); resolve(); }, 6000);
    client.startDeviceScan(null, { allowDuplicates: false }, (error, device) => {
      if (error) { clearTimeout(timer); client.stopDeviceScan(); reject(error); return; }
      const name = device?.name || device?.localName;
      const hasNrfService = device?.serviceUUIDs?.some((uuid) =>
        /^(?:0000)?fff0(?:-0000-1000-8000-00805f9b34fb)?$/i.test(uuid));
      if (device && name &&
          (/^NRF_[A-F0-9]{12}$/i.test(name) ||
           (/^PROV_[A-F0-9]{12}$/i.test(name) && hasNrfService))) {
        found.set(device.id, new NrfProvisioningDevice(device.id, name));
      }
    });
  });
  return [...found.values()];
}
