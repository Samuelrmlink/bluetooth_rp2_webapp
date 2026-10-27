// Toolbar buttons for another web app.
//
// Two ways onto the same list:
//
//   * A folder manifest. LittleFS apps/<id>/app.json opens /apps/<id>/.
//     The card's apps/<id>/app.json opens /sd/apps/<id>/. The link is the
//     folder. The file's label is the button text (title is the old name
//     for the same field). A folder with no manifest is not a button, and
//     the file cannot choose a different URL.
//   * app.addLauncher({id, label, href}) or {id, label, onClick}, from code
//     that is already running in this page (a js/ext module). href has to
//     be a path on this device. onClick is used only when there is no href.
//
// list and readText are passed in, the same way extensions.js takes them,
// so a test never touches the network.

export const APP_ID = /^[A-Za-z0-9_-]+$/;
export const LABEL_MAX = 40;

const CONTROLS = /[\u0000-\u001f\u007f]/g;
const HAS_CONTROL = /[\u0000-\u001f\u007f]/;

// Folder names from a one-level listing, in order. Pure.
export function appNames(files) {
  return (files || []).filter((f) => f && f.dir && APP_ID.test(f.name)).map((f) => f.name).sort();
}

// Plain text, trimmed, capped. An empty result uses the id.
export function cleanLabel(raw, id) {
  const text = (typeof raw === 'string' ? raw : '').replace(CONTROLS, '').trim();
  return text.slice(0, LABEL_MAX).trim() || id;
}

// null when the text is not an object. href in the file is ignored.
export function parseLabel(text, id) {
  let obj;
  try {
    obj = JSON.parse(text);
  } catch {
    return null;
  }
  if (!obj || typeof obj !== 'object' || Array.isArray(obj)) return null;
  const raw = typeof obj.label === 'string' ? obj.label : typeof obj.title === 'string' ? obj.title : '';
  return cleanLabel(raw, id);
}

export function hrefFor(volume, id) {
  const name = encodeURIComponent(id);
  return volume === 'sd' ? `/sd/apps/${name}/` : `/apps/${name}/`;
}

// A same-origin path: one leading slash, never a protocol-relative URL.
export function validHref(href) {
  return typeof href === 'string'
    && href.startsWith('/')
    && !href.startsWith('//')
    && !HAS_CONTROL.test(href);
}

// The in-page registration. href wins over onClick when both are set.
export function normalizeLauncher(spec) {
  const id = spec?.id;
  if (typeof id !== 'string' || !APP_ID.test(id)) return { ok: false, error: 'bad id' };
  const label = cleanLabel(spec.label, id);
  if (spec.href != null && spec.href !== '') {
    if (!validHref(spec.href)) return { ok: false, error: 'href must be a path on this device' };
    return { ok: true, launcher: { id, label, href: spec.href, onClick: null, volume: 'api' } };
  }
  if (typeof spec.onClick === 'function') {
    return { ok: true, launcher: { id, label, href: null, onClick: spec.onClick, volume: 'api' } };
  }
  return { ok: false, error: 'needs an href or onClick' };
}

function fit(label, suffix) {
  const extra = ` (${suffix})`;
  const room = Math.max(1, LABEL_MAX - extra.length);
  return label.slice(0, room).trimEnd() + extra;
}

// One button per app. The same folder name on both volumes stays two
// buttons; matching labels gain (device) and (card), and the card's id
// gains an sd- prefix so the two can share the list.
export function collate(lfs, sd) {
  const lfsBy = new Map((lfs || []).map((a) => [a.id, a]));
  const sdBy = new Map((sd || []).map((a) => [a.id, a]));
  const out = [];
  const taken = new Set();
  for (const a of lfs || []) {
    const other = sdBy.get(a.id);
    const rec = {
      id: a.id,
      label: other && other.label === a.label ? fit(a.label, 'device') : a.label,
      href: hrefFor('lfs', a.id),
      onClick: null,
      volume: 'lfs',
    };
    taken.add(rec.id);
    out.push(rec);
  }
  for (const a of sd || []) {
    const other = lfsBy.get(a.id);
    let id = other ? `sd-${a.id}` : a.id;
    if (taken.has(id)) id = `sd-${a.id}-card`;
    taken.add(id);
    out.push({
      id,
      label: other && other.label === a.label ? fit(a.label, 'card') : a.label,
      href: hrefFor('sd', a.id),
      onClick: null,
      volume: 'sd',
    });
  }
  return out;
}

async function readApp(readText, dir, id) {
  const plain = await readText(`${dir}/app.json`);
  if (plain?.ok) {
    const label = parseLabel(plain.text, id);
    return label == null ? { error: 'bad app.json' } : { label };
  }
  if (plain?.status !== 404) return { error: plain?.error || 'read failed' };
  const gz = await readText(`${dir}/app.json.gz`);
  if (gz?.ok) {
    const label = parseLabel(gz.text, id);
    return label == null ? { error: 'bad app.json' } : { label };
  }
  if (gz?.status !== 404) return { error: gz?.error || 'read failed' };
  return { missing: true };
}

async function loadVolume(listDir, readText, dir, log) {
  let listed;
  try {
    listed = await listDir(dir);
  } catch (e) {
    log('!', `launchers ${dir}: ${e?.message || e}`);
    return { ok: false };
  }
  if (!listed?.ok) {
    if (listed?.status === 404) return { ok: true, apps: [] };
    log('!', `launchers ${dir}: ${listed?.error || 'unavailable'}`);
    return { ok: false };
  }
  const apps = [];
  for (const name of appNames(listed.files)) {
    let got;
    try {
      got = await readApp(readText, `${dir}/${name}`, name);
    } catch (e) {
      log('!', `launcher ${name}: ${e?.message || e}`);
      continue;
    }
    if (got.missing) continue;
    if (got.error) {
      log('!', `launcher ${name}: ${got.error}`);
      continue;
    }
    apps.push({ id: name, label: got.label });
  }
  return { ok: true, apps };
}

// lfs and sd are the new records for that volume, or null when that
// listing failed and the caller should keep what it already showed.
export async function loadFolderLaunchers({ listDir, readText, sd = false, log = () => {} } = {}) {
  const lfs = await loadVolume(listDir, readText, 'apps', log);
  const card = sd ? await loadVolume(listDir, readText, 'sd/apps', log) : { ok: true, apps: [] };
  const both = collate(lfs.ok ? lfs.apps : [], card.ok ? card.apps : []);
  return {
    lfs: lfs.ok ? both.filter((a) => a.volume === 'lfs') : null,
    sd: card.ok ? both.filter((a) => a.volume === 'sd') : null,
  };
}

export function createRegistry() {
  const entries = new Map();
  return {
    has: (id) => entries.has(id),
    get: (id) => entries.get(id),
    list() {
      return [...entries.values()].sort((a, b) => a.label.localeCompare(b.label) || a.id.localeCompare(b.id));
    },
    // True when an existing launcher was replaced by a different one.
    set(rec) {
      const prev = entries.get(rec.id);
      entries.set(rec.id, rec);
      if (!prev) return false;
      if (prev.onClick || rec.onClick) return true;
      return prev.label !== rec.label || prev.href !== rec.href || prev.volume !== rec.volume;
    },
    remove: (id) => entries.delete(id),
    // Drop this volume's buttons, then add the new ones. Ids that overwrote
    // some other volume are returned so the caller can log them.
    replaceVolume(volume, apps) {
      const overwritten = [];
      for (const [id, rec] of [...entries]) {
        if (rec.volume === volume) entries.delete(id);
      }
      for (const rec of apps || []) {
        if (entries.has(rec.id)) overwritten.push(rec.id);
        entries.set(rec.id, rec);
      }
      return overwritten;
    },
  };
}
