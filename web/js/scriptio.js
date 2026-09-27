/*
 * The draft/stage/commit protocol, without a DOM.
 *
 * Both directions have to be paged, and for the same reason in reverse: the
 * per-connection WebSocket receive buffer is small on purpose, because it is
 * multiplied by the connection count. So a draft goes UP in chunks and source
 * comes DOWN in chunks, and neither side may assume one frame is enough. Get
 * that wrong and the failure is silent truncation, which is why the logic
 * lives here where it can be tested against a fake transport rather than in a
 * renderer where it can only be tested by hand.
 *
 * Nothing here touches the DOM or a real socket; it takes anything with
 * .call(op, params) returning a promise.
 */

/* Conservative against the firmware's WS_RX_MAX. The reply must also fit, and
 * escaping can double a chunk of newlines and quotes -- Lua source is full of
 * both -- so the wire cost of a chunk is up to twice its length. */
export const DRAFT_CHUNK = 600;

/** Split source into the chunks script.draft expects. Always at least one,
 *  so an EMPTY draft is still a draft -- that is how you blank a script. */
export function draftChunks(source, size = DRAFT_CHUNK) {
  const out = [];
  for (let off = 0; off < source.length; off += size) {
    const part = source.slice(off, off + size);
    out.push({ offset: off, source: part, final: off + part.length >= source.length });
  }
  if (out.length === 0) out.push({ offset: 0, source: '', final: true });
  return out;
}

export class ScriptIO {
  constructor(api) { this.api = api; }

  /** Push a draft of any size. RAM only -- nothing is written to flash. */
  async draft(script, source, { run = true, onProgress } = {}) {
    const chunks = draftChunks(source);
    let last = null;
    for (let i = 0; i < chunks.length; i++) {
      const c = chunks[i];
      last = await this.api.call('script.draft', {
        script, ...c,
        // Only the final chunk may run, or a half-uploaded script executes.
        run: c.final ? run : false,
      });
      onProgress?.(i + 1, chunks.length);
    }
    return last;
  }

  /** Read back whatever tier is in effect. Returns {source, tier}. */
  async source(script, { onProgress } = {}) {
    let offset = 0, out = '', tier = null, guard = 0;
    for (;;) {
      const r = await this.api.call('script.source', { script, offset });
      tier = r.tier;
      out += r.chunk ?? '';
      onProgress?.(out.length, r.length ?? 0);
      if (r.eof || !r.bytes) break;
      offset += r.bytes;
      // A device that never sets eof would spin here forever. 4 MB is far
      // past any plausible script and well past the filesystem.
      if (++guard > 8192) throw new Error('source read did not terminate');
    }
    return { source: out, tier };
  }
}

/* Merge what the device HAS with what it is RUNNING.
 *
 * script.list answers both halves separately and they overlap: a name can be
 * a committed file, a staged copy, and a live runtime slot holding a RAM
 * draft, all at once. The tier that wins is the tier that would actually run,
 * which is the precedence resolve_src() applies on the device:
 *
 *     draft  >  staged  >  committed
 *
 * Getting this wrong is not cosmetic. Showing "committed" while a draft is in
 * effect means the editor loads one thing and the board runs another, which
 * is the single worst failure this UI can have.
 *
 * A MODULE is not startable. It is required by a script, has no runtime slot
 * of its own, and offering Start for one would produce a button that can only
 * fail.
 */
export function mergeScripts(list) {
  const byName = new Map();
  const rank = { committed: 0, staged: 1, draft: 2 };

  for (const f of list?.files || []) {
    const prev = byName.get(f.name);
    const entry = {
      name: f.name,
      kind: f.kind === 'module' ? 'module' : 'script',
      tier: f.tier,
      bytes: f.bytes,
      running: false, dead: false, draft: false, resumes: 0, error: '',
    };
    // staged/<name>.lua and lua/<name>.lua are the same script at two tiers.
    if (!prev || rank[f.tier] > rank[prev.tier]) {
      byName.set(f.name, prev ? { ...prev, tier: f.tier, bytes: f.bytes } : entry);
    }
  }

  for (const s of list?.scripts || []) {
    const prev = byName.get(s.name);
    const merged = {
      ...(prev || { name: s.name, kind: 'script', tier: 'committed', bytes: null }),
      running: !!s.running, dead: !!s.dead, draft: !!s.draft,
      resumes: s.resumes ?? 0, error: s.error || '',
    };
    // A live draft outranks anything on flash, and a runtime slot for a name
    // with no file at all is a draft that has never been staged.
    if (s.draft || !prev) merged.tier = s.draft ? 'draft' : merged.tier;
    byName.set(s.name, merged);
  }

  const order = { script: 0, module: 1 };
  return [...byName.values()].sort((a, b) =>
    (order[a.kind] - order[b.kind]) || a.name.localeCompare(b.name));
}

/** Only a top-level script can be started; a module is required, not run. */
export function isStartable(entry) {
  return !!entry && entry.kind === 'script';
}
