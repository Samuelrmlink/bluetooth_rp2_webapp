// WS2812B strip. The PEX module registers ws2812.set and publishes
// profile.ws2812.telemetry while it is running. The strip stays dark
// until On: loading the module does not claim a state machine.
// GP22 is the data pin on this board (SD, the radio, and the W5500 are not).

export const controls = [
  { kind: 'switch', field: 'on', label: 'On' },
  { kind: 'number', field: 'pin', label: 'Data pin', min: 0, max: 29, step: 1 },
  { kind: 'number', field: 'count', label: 'LEDs', min: 1, max: 65535, step: 1, live: true },
  { kind: 'select', field: 'mode', label: 'Mode', options: [
    ['solid', 'Solid'],
    ['rainbow', 'Rainbow'],
    ['breathe', 'Breathe'],
    ['chase', 'Chase'],
  ] },
  { kind: 'slider', field: 'hue', label: 'Hue', min: 0, max: 359, step: 1, unit: '°' },
  { kind: 'slider', field: 'sat', label: 'Saturation', min: 0, max: 255, step: 1 },
  { kind: 'slider', field: 'val', label: 'Brightness', min: 0, max: 255, step: 1 },
  { kind: 'slider', field: 'speed', label: 'Speed', min: 0, max: 100, step: 1 },
  { kind: 'readout', field: 'running', label: 'PIO', format: (v) => (Number(v) ? 'claimed' : 'idle') },
  { kind: 'readout', field: 'status', label: 'State' },
];

export function write(ctx, control, value) {
  return ctx.ws.call('ws2812.set', { [control.field]: value });
}
