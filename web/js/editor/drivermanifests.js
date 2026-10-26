// Scanner manifests written when a PEX module is installed.
//
// pex/modules/drivers/anker.driver.json is the canonical copy in the repo.
// A successful install writes pex/<id>.driver.json on the device, which the
// scanner reads before any Lua profile. The page cannot read the repo at
// runtime, so the text is kept here and the unit test checks it still matches.

const ANKER_JSON = `{
  "id": "anker",
  "title": "Anker SOLIX / Prime",
  "kind": "pex",
  "file": "pex/anker.m33.pex",
  "note": "Company ffff and service ff09. The charger's advertised name is its serial, so a name rule would miss it.",
  "match": [
    { "all": [ { "mfg_company": "ffff" }, { "uuid16": "ff09" } ] }
  ]
}
`;

const MANIFESTS = { anker: ANKER_JSON };

/** The manifest object, or null when this module has none. */
export function manifestFor(name) {
  const text = MANIFESTS[name];
  return text ? JSON.parse(text) : null;
}

export function manifestPath(manifest) {
  return `pex/${manifest.id}.driver.json`;
}

/**
 * Write pex/<id>.driver.json. `write` is the editor's fs.write.
 * An unknown name is a skip, not a failure. A write error is returned
 * and does not undo an install that already succeeded.
 */
export async function deployDriverManifest(write, name) {
  const text = MANIFESTS[name];
  if (!text) return { ok: true, skipped: true };
  const manifest = JSON.parse(text);
  const path = manifestPath(manifest);
  const body = text.endsWith('\n') ? text : `${text}\n`;
  const r = await write(path, body);
  if (!r?.ok) return { ok: false, error: r.error || `could not write ${path}`, path };
  return { ok: true, path };
}
