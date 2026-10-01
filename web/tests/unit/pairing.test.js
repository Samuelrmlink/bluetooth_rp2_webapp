import { test } from 'node:test';
import assert from 'node:assert/strict';
import { promptFor, six } from '../../js/pairing.js';

test('passkeys show as six digits', () => {
  assert.equal(six(42), '000042');
  assert.equal(six(123456), '123456');
});

test('each firmware question becomes the right prompt', () => {
  assert.equal(promptFor('security.passkey_request', { addr: 'AA' }).kind, 'input');
  const c = promptFor('security.numeric_comparison', { addr: 'AA', number: 7 });
  assert.deepEqual([c.kind, c.number], ['compare', '000007']);
  const d = promptFor('security.passkey_display', { addr: 'AA', number: 999999 });
  assert.deepEqual([d.kind, d.number], ['display', '999999']);
  assert.equal(promptFor('security.bonded', {}), null);
});
