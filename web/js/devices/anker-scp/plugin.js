// Anker small chargers (the A2687 family). Field names from
// lua/profiles/anker/models/A2687.lua. Readouts only until a write path is
// added to the driver and verified on the charger.

export const controls = [
  { kind: 'readout', field: 'c1_power', label: 'USB-C1' },
  { kind: 'readout', field: 'c2_power', label: 'USB-C2' },
  { kind: 'readout', field: 'c3_power', label: 'USB-C3' },
  { kind: 'readout', field: 'charging_protocol', label: 'Protocol' },
  { kind: 'readout', field: 'backlight_pct', label: 'Backlight', unit: '%' },
  { kind: 'readout', field: 'error_code', label: 'Error code' },
];
