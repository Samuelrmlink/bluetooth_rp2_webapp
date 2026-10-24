import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { sha256 } from '../../js/editor/sha256.js';

function hex(bytes) {
  return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');
}

test('sha256 matches the known vectors and node crypto, with no subtle', () => {
  assert.equal(hex(sha256(new Uint8Array())), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  assert.equal(hex(sha256(new TextEncoder().encode('abc'))), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  const blob = new Uint8Array(1000);
  for (let i = 0; i < blob.length; i++) blob[i] = i & 0xff;
  assert.equal(hex(sha256(blob)), createHash('sha256').update(blob).digest('hex'));
  const saved = globalThis.crypto;
  // The linker must not need subtle. Hiding it here must not change the digest.
  Object.defineProperty(globalThis, 'crypto', { value: undefined, configurable: true });
  try {
    assert.equal(hex(sha256(blob)), createHash('sha256').update(blob).digest('hex'));
  } finally {
    Object.defineProperty(globalThis, 'crypto', { value: saved, configurable: true });
  }
});
