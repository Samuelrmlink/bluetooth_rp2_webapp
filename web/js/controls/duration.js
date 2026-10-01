// Durations as people type them, and as a countdown shows them. DOM-free.
//
//   1:30      1 h 30 min (h:mm -- this is for timers, hours come first)
//   1:30:15   h:mm:ss
//   90m 2h 45s 1h30m      units, any order of h / m / s
//   45        a bare number is minutes
//
// Returns whole seconds, or null for anything else.
export function parseDuration(text) {
  const s = String(text ?? '').trim().toLowerCase().replace(/\s+/g, '');
  if (!s) return null;
  let m = /^(\d+):([0-5]?\d)(?::([0-5]?\d))?$/.exec(s);
  if (m) return Number(m[1]) * 3600 + Number(m[2]) * 60 + Number(m[3] || 0);
  if (/^\d+(\.\d+)?$/.test(s)) return Math.round(Number(s) * 60);
  m = /^(?:(\d+(?:\.\d+)?)h)?(?:(\d+(?:\.\d+)?)m(?:in)?)?(?:(\d+)s)?$/.exec(s);
  if (m && (m[1] || m[2] || m[3])) {
    return Math.round(Number(m[1] || 0) * 3600 + Number(m[2] || 0) * 60 + Number(m[3] || 0));
  }
  return null;
}

// h:mm:ss, never negative.
export function formatDuration(secs) {
  const t = Math.max(0, Math.floor(Number(secs) || 0));
  const h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), s = t % 60;
  return `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}
