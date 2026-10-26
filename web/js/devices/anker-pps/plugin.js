// Anker portable power stations: the SOLIX C1000 family (A1761; A1753,
// A1770 and A1771 share its layout). Field names are the PEX model table's
// (pex/modules/drivers/anker_models.c), read off a decompiled app and
// verified field by field only as far as the notes there say.
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
  { kind: 'readout', field: 'ac_output_power_w', label: 'AC output power', unit: 'W' },
  { kind: 'readout', field: 'ac_input_power_w', label: 'AC input power', unit: 'W' },
  { kind: 'readout', field: 'solar_input_power_w', label: 'Solar input power', unit: 'W' },
  { kind: 'readout', field: 'bms_temp_major_c', label: 'Battery temperature', unit: '°C' },

  // Writable: the four setters whose payloads were captured from the app
  // (A1761 rows in anker_models.c). `verified` is set once a
  // change from here has been seen to move the matching telemetry field on
  // the bench C1000.
  { kind: 'switch', field: 'car_output_enabled', label: 'Car output', verified: true },
  // Verified 2026-09-30: off from here switched the outlet off (seen by the
  // owner; the network it powers dropped, so no telemetry got out). On was
  // switched by hand afterwards. Asks first: off can cut this gateway's link.
  { kind: 'switch', field: 'ac_output_enabled', label: 'AC output', verified: true,
    confirm: { 0: 'Switch the AC outlet OFF?\n\nEverything plugged into it loses power. If the gateway or its network is powered from it, this page loses the connection and the outlet must be switched back on at the C1000.' } },
  { kind: 'switch', field: 'screen_enabled', label: 'Display', verified: true },
  { kind: 'select', field: 'ambient_light', label: 'Ambient light', verified: true,
    options: [[0, 'Off'], [1, 'On'], [3, '3 (captured; meaning unknown)']] },
  // Shutoff timers: the output switches off when one runs out. Seconds on
  // the wire (0x0042 / 0x0043, read back on a2 / a3); 0 cancels. A sample
  // lands a second or two after the write, by when the timer has ticked, so
  // it confirms within `tolerance` seconds of what was sent.
  { kind: 'timer', field: 'ac_countdown_s', label: 'AC timer', max: 86400, tolerance: 20, verified: true },
  { kind: 'timer', field: 'car_countdown_s', label: 'DC timer', max: 86400, tolerance: 20, verified: true },
  // 0x0044, u16 watts, captured by the owner 2026-10-01 (700 W); read back on d1.
  { kind: 'number', field: 'ac_charge_power_w', label: 'AC charge limit', unit: 'W', min: 100, max: 1300, step: 100,
    verified: true },

  // Opcodes known, payloads never captured: shown, not writable. A guessed
  // encoding is not something to try on a power station.
  { kind: 'select', field: 'ac_frequency_hz', label: 'AC frequency', options: [[50, '50 Hz'], [60, '60 Hz']],
    blocked: 'setter payload not yet captured' },
  { kind: 'select', field: 'device_timeout_min', label: 'Device auto-off',
    options: [[0, 'Never'], [30, '30 min'], [60, '1 h'], [120, '2 h'], [360, '6 h'], [720, '12 h'], [1440, '24 h']],
    blocked: 'setter payload not yet captured' },
  { kind: 'switch', field: 'ultra_fast_charging', label: 'Ultra-fast charging', blocked: 'no setter known' },
];

// A script op reports a refusal as a result carrying `error`, not as a
// failed call -- so it is turned into one here, or the control would sit
// pending until it timed out instead of saying why at once.
export async function write(ctx, control, value) {
  const r = await ctx.ws.call('anker.set', { device: ctx.device.addr, field: control.field, value });
  if (r && r.error) throw new Error(r.message || r.error);
  return r;
}
