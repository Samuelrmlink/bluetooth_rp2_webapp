// The Pinecil soldering iron. Its driver (lua/profiles/pinecil.lua)
// registers pinecil.set_temp, which the old Pinecil page drove -- so the
// setpoint is a working control from the start.

export const controls = [
  { kind: 'readout', field: 'tip_c', label: 'Tip', unit: '°C', digits: 0 },
  { kind: 'number', field: 'setpoint_c', label: 'Setpoint', unit: '°C', min: 10, max: 450, step: 5 },
  { kind: 'readout', field: 'power_pct', label: 'Power', unit: '%' },
  { kind: 'readout', field: 'watts', label: 'Watts', unit: 'W', digits: 1 },
  { kind: 'readout', field: 'input_v', label: 'Input', unit: 'V', digits: 1 },
  { kind: 'readout', field: 'handle_c', label: 'Handle', unit: '°C', digits: 0 },
  { kind: 'readout', field: 'mode', label: 'Mode' },
];

export function write(ctx, control, value) {
  if (control.field !== 'setpoint_c') return Promise.reject(new Error('not writable'));
  return ctx.ws.call('pinecil.set_temp', { celsius: value });
}
