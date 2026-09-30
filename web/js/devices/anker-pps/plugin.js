// Anker portable power stations: the SOLIX C1000 family (A1761; A1753,
// A1770 and A1771 share its layout). Field names are the Lua schema's
// (lua/profiles/anker/models/A1761.lua), which are read off a decompiled app
// and verified field by field only as far as the notes there say.
//
// Controls that WRITE go through the driver's anker.set op. Each ships
// disabled (verified: false) until it has been changed on the bench C1000
// and the matching telemetry field seen to follow -- the model file's own
// rule for promoting a field.

export const css = 'plugin.css';

export const controls = [
  { kind: 'readout', field: 'battery_pct', label: 'Battery', unit: '%' },
  { kind: 'readout', field: 'remaining_time_h', label: 'Remaining', unit: 'h', digits: 1 },
  { kind: 'readout', field: 'total_output_power_w', label: 'Output, total', unit: 'W' },
  { kind: 'readout', field: 'total_input_power_w', label: 'Input, total', unit: 'W' },
  { kind: 'readout', field: 'ac_output_power_w', label: 'AC output', unit: 'W' },
  { kind: 'readout', field: 'ac_input_power_w', label: 'AC input', unit: 'W' },
  { kind: 'readout', field: 'solar_input_power_w', label: 'Solar input', unit: 'W' },
  { kind: 'readout', field: 'bms_temp_major_c', label: 'Battery temperature', unit: '°C' },

  { kind: 'switch', field: 'ac_output_enabled', label: 'AC output', verified: false },
  { kind: 'switch', field: 'car_output_enabled', label: 'Car output', verified: false },
  { kind: 'select', field: 'ac_frequency_hz', label: 'AC frequency', options: [[50, '50 Hz'], [60, '60 Hz']], verified: false },
  { kind: 'number', field: 'ac_charge_power_w', label: 'AC charge limit', unit: 'W', min: 100, max: 1300, step: 100, verified: false },
  { kind: 'select', field: 'device_timeout_min', label: 'Device auto-off',
    options: [[0, 'Never'], [30, '30 min'], [60, '1 h'], [120, '2 h'], [360, '6 h'], [720, '12 h'], [1440, '24 h']], verified: false },
  { kind: 'switch', field: 'ultra_fast_charging', label: 'Ultra-fast charging', verified: false },
];

export function write(ctx, control, value) {
  return ctx.ws.call('anker.set', { device: ctx.device.addr, field: control.field, value });
}
