// Has the file on the device changed since this buffer was loaded?
//
// The editor stores FNV-1a of the text it last read or saved (b.base).
// Neither filesystem has a modification time the page can trust: LittleFS
// stores none, and the card's FatFs is built without a clock, so every
// entry is stamped the same day. A hash of the bytes the editor would show
// is the comparison. A .gz is fetched as text (Content-Encoding: gzip),
// which is the same text b.base was computed from.

export const FRESH_MS = 5000;
export const FRESH_BACKOFF_MAX = 30000;

// Delay before the next check after the card answers 503. Doubles, and
// stops at 30s. `previous` is 0 for the first busy answer.
export function nextFreshDelay(previous) {
  const n = previous ? Math.min(previous * 2, FRESH_BACKOFF_MAX) : 10000;
  return n;
}

// What to do with one open buffer.
// remote.status 200 carries remote.hash (fnv1a of the text a reload would
// show). 404 means the file is gone. 503 means the card is busy.
// Returns 'skip' | 'busy' | 'error' | 'same' | 'reload' | 'ask' | 'gone' | 'gone-dirty'.
export function freshnessAction({ binary, partition, isNew, pristine, dirty, base, kept, notedGone, askedGone, remote }) {
  if (binary || partition) return 'skip';
  if (!remote || remote.offline) return 'error';
  if (remote.status === 503) return 'busy';
  if (remote.status === 404 || remote.missing) {
    if (isNew || pristine || base === '') return 'same';
    if (dirty) return notedGone ? 'same' : 'gone-dirty';
    return askedGone ? 'same' : 'gone';
  }
  if (remote.status !== 200 || remote.hash == null) return 'error';
  if (remote.hash === base) return 'same';
  if (pristine) return 'reload';
  if (dirty) return remote.hash === kept ? 'same' : 'ask';
  return 'reload';
}
