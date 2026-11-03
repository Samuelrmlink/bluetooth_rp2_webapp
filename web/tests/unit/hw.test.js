import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pinClass, pinTitle, summarise, fmtI2cResults, fmtVolts, fmtCelsius, ADC_CHANNELS, pioView, pioProgLine }
  from '../../js/hw.js';

/* The real map off the board: 30 pins, four reserved by the radio. */
const PINS = [
  ...Array.from({ length: 30 }, (_, pin) => ({ pin, state: 'free', function: 'none', owner: '' })),
];
for (const p of [23, 24, 25, 29]) {
  PINS[p] = { pin: p, state: 'reserved', function: 'none', owner: 'cyw43' };
}

test('the board reports 30 pins with four reserved by the radio', () => {
  const s = summarise(PINS);
  assert.equal(s.total, 30);
  assert.equal(s.reserved, 4);
  assert.equal(s.free, 26);
  assert.equal(s.claimed, 0);
});

test('reserved is its own class, distinct from claimed', () => {
  // Rendering a radio pin as merely "busy" invites someone to free it.
  assert.equal(pinClass({ state: 'reserved' }), 'reserved');
  assert.equal(pinClass({ state: 'free' }), 'free');
  assert.equal(pinClass({ state: 'claimed' }), 'claimed');
  assert.equal(pinClass(null), 'unknown');
});

test('a radio pin explains the consequence, not just the state', () => {
  const t = pinTitle(PINS[23]);
  assert.match(t, /reserved by cyw43/);
  assert.match(t, /WiFi and Bluetooth/);
});

test('a claimed pin names its owner and function', () => {
  const t = pinTitle({ pin: 4, state: 'claimed', owner: 'i2c0', function: 'sda' });
  assert.match(t, /claimed by i2c0/);
  assert.match(t, /as sda/);
});

test('i2c results keep only valid 7-bit addresses', () => {
  // 0x00 is the general-call address and 0x78+ is reserved; either coming
  // back means the scan is wrong, not that something answered.
  const r = fmtI2cResults([0x00, 0x3c, 0x76, 0x7f]);
  assert.deepEqual(r.list.map((d) => d.hex), ['0x3c', '0x76']);
  assert.equal(r.note, '2 devices');
});

test('an empty i2c bus says so rather than showing nothing', () => {
  assert.equal(fmtI2cResults([]).note, 'nothing responded');
  assert.equal(fmtI2cResults(undefined).note, 'no result');
});

test('one device is not "1 devices"', () => {
  assert.equal(fmtI2cResults([0x48]).note, '1 device');
});

test('adc channels name the contended one', () => {
  const ch3 = ADC_CHANNELS.find((c) => c.ch === 3);
  assert.equal(ch3.pin, 29);
  assert.match(ch3.note, /radio/);
  const temp = ADC_CHANNELS.find((c) => c.ch === 4);
  assert.equal(temp.pin, null);
});

test('pio status names the owner of each taken instruction word', () => {
  const v = pioView({
    blocks: [{
      pio: 2,
      gpio_base: 0,
      instr_used: 17,
      instr_free: 15,
      instr_map: (1 << 17) - 1,
      sm_free: 0b1010,
      irq_used: 0,
      irq_lines: 1,
      programs: [
        { owner: 'cyw43', offset: 0, length: 6, origin: -1, sm: 1, irq: 0, prot: 1 },
        { owner: 'sd', offset: 6, length: 11, origin: -1, sm: 4, irq: 0, prot: 1 },
      ],
    }],
  });
  assert.equal(v.blocks.length, 1);
  assert.equal(v.blocks[0].cells[0].owner, 'cyw43');
  assert.equal(v.blocks[0].cells[5].owner, 'cyw43');
  assert.equal(v.blocks[0].cells[5].prot, true);
  assert.equal(v.blocks[0].cells[6].owner, 'sd');
  assert.equal(v.blocks[0].cells[6].prot, true);
  assert.equal(v.blocks[0].cells[16].owner, 'sd');
  assert.equal(v.blocks[0].cells[17].taken, false);
  assert.deepEqual(v.blocks[0].sms.filter((s) => s.free).map((s) => s.sm), [1, 3]);
  assert.deepEqual(v.blocks[0].sms[0].owners, ['cyw43']);
  assert.deepEqual(v.blocks[0].irq_lines, [0]);
  assert.match(v.summary, /17 of 32 words/);
  assert.match(v.summary, /2 machines/);
  assert.match(pioProgLine(v.blocks[0].programs[0]), /cyw43/);
  assert.match(pioProgLine(v.blocks[0].programs[0]), /protected/);
  assert.match(pioProgLine(v.blocks[0].programs[1]), /words 6–16/);
  assert.match(pioProgLine(v.blocks[0].programs[1]), /protected/);
  assert.match(pioProgLine(v.blocks[0].programs[1]), /origin auto/);
});

test('a taken word with no program is unknown, and bit 31 still counts', () => {
  const v = pioView({ blocks: [{ instr_map: 2147483648, programs: [], sm_free: 0 }] });
  assert.equal(v.blocks[0].cells[31].taken, true);
  assert.equal(v.blocks[0].cells[31].owner, '?');
  assert.equal(v.blocks[0].cells[30].taken, false);
  assert.equal(v.blocks[0].sms.filter((s) => s.free).length, 0);
});

test('a missing pio reply is an empty status, not a throw', () => {
  const v = pioView(undefined);
  assert.deepEqual(v.blocks, []);
  assert.equal(v.summary, 'no PIO status');
});

test('missing readings render as absent, not zero', () => {
  assert.equal(fmtVolts(undefined), '—');
  assert.equal(fmtCelsius(null), '—');
  assert.equal(fmtVolts(0.6969), '0.697 V');
  assert.equal(fmtCelsius(32.29), '32.3 °C');
});
