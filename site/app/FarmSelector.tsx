"use client";

export type FarmOption = { $id: string; name: string };

export const selectedFarmStorageKey = (userId: string) => `live-stocking:selected-farm:${userId}`;

export function preferredFarmId(userId: string, farms: FarmOption[], accountPreference = "") {
  const stored = window.localStorage.getItem(selectedFarmStorageKey(userId)) || "";
  return [stored, accountPreference, farms[0]?.$id || ""].find((candidate) =>
    farms.some((farm) => farm.$id === candidate)) || "";
}

export default function FarmSelector({ farms, value, onChange }: {
  farms: FarmOption[];
  value: string;
  onChange: (farmId: string) => void;
}) {
  return <label className="farm-selector">
    <span>Farm</span>
    <select aria-label="Current farm" value={value} onChange={(event) => onChange(event.target.value)} disabled={!farms.length}>
      {!farms.length && <option value="">No farms</option>}
      {farms.map((farm) => <option key={farm.$id} value={farm.$id}>{farm.name}</option>)}
    </select>
  </label>;
}
