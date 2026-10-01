// Anker small chargers (the A2687 family). Field names from
// lua/profiles/anker/models/A2687.lua. Readouts only until a write path is
// added to the driver and verified on the charger.

// A port's power is { mv, ma, watts, charging }; its control is { on, cable,
// countdown_s, countdown_remain_s, countdown_active, vid }.
export function portPower(v) {
  if (!v || typeof v !== 'object') return '\u2014';
  if (!v.charging && !v.mv) return 'idle';
  return `${(v.mv / 1000).toFixed(2)} V \u00b7 ${(v.ma / 1000).toFixed(2)} A \u00b7 ${Number(v.watts).toFixed(1)} W`;
}

export function portControl(v) {
  if (!v || typeof v !== 'object') return '\u2014';
  const parts = [v.on ? 'on' : 'off'];
  if (v.cable && v.cable !== 'none') parts.push(`${v.cable} cable`);
  if (v.countdown_active) parts.push(`off in ${Math.round(v.countdown_remain_s / 60)} min`);
  return parts.join(' \u00b7 ');
}

export const controls = [
  { kind: 'readout', field: 'c1_power', label: 'USB-C1', format: portPower },
  { kind: 'readout', field: 'c2_power', label: 'USB-C2', format: portPower },
  { kind: 'readout', field: 'c3_power', label: 'USB-C3', format: portPower },
  { kind: 'readout', field: 'c1_control', label: 'USB-C1 port', format: portControl },
  { kind: 'readout', field: 'c2_control', label: 'USB-C2 port', format: portControl },
  { kind: 'readout', field: 'c3_control', label: 'USB-C3 port', format: portControl },
  { kind: 'readout', field: 'fixed_allocation', label: 'Allocation' },
  { kind: 'readout', field: 'backlight_pct', label: 'Backlight', unit: '%' },
  { kind: 'readout', field: 'screen_off', label: 'Screen off after' },
  { kind: 'readout', field: 'error_code', label: 'Error code' },
];
