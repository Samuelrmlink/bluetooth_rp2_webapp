/*
 * The Script pane: edit Lua on the device, from the device.
 *
 * This is the pane the whole architecture exists for. Reverse-engineering a
 * protocol is a high-churn activity, and the point of putting the churn in
 * Lua was to get it out from behind a flash erase cycle -- but until there is
 * an editor, "editable over the websocket" means curl and a shell, which is
 * not the same claim.
 *
 * The three tiers are the model, so the UI shows them rather than hiding
 * them: a DRAFT runs now and never touches flash, STAGED survives a reboot
 * with the original intact, COMMITTED is the real file. The Save button is
 * deliberately three buttons, because which one you mean is the whole point.
 *
 * Renderer only. The paging in both directions lives in scriptio.js, where it
 * is tested.
 */
import { ScriptIO } from '../scriptio.js';

const $ = (id) => document.getElementById(id);

export function mountScriptPane({ api, log }) {
  const io = new ScriptIO(api);
  let current = null;             // script name being edited
  let dirty = false;
  let lastLoadedTier = null;
  const errors = [];

  const el = {
    sel: $('s-script'), src: $('s-src'), tier: $('s-tier'), stat: $('s-stat'),
    errs: $('s-errors'), evalIn: $('s-eval'), evalOut: $('s-eval-out'),
  };

  function setStatus(text, kind = '') {
    el.stat.textContent = text;
    el.stat.dataset.kind = kind;
  }

  /* ---- listing ---------------------------------------------------------- */
  async function refresh() {
    let r;
    try { r = await api.call('script.list'); }
    catch (e) { setStatus(e.message, 'err'); return; }

    const names = (r.scripts || []).map(s => s.name);
    // Keep whatever is typed in: the list is what the DEVICE knows about, and
    // a new script has no entry until its first draft.
    const want = current || el.sel.value;
    el.sel.replaceChildren(...names.map(n => {
      const o = document.createElement('option');
      o.value = n; o.textContent = n;
      return o;
    }));
    if (names.includes(want)) el.sel.value = want;

    const s = (r.scripts || []).find(x => x.name === (current || el.sel.value));
    const rt = r.runtime || {};
    const bits = [];
    if (s) {
      bits.push(s.dead ? 'dead' : s.running ? 'running' : 'stopped');
      if (s.draft) bits.push('has draft');
      bits.push(`${s.resumes} resumes`);
      if (s.error) pushError(s.name, s.error);
    }
    if (rt.arena_bytes) {
      const pct = Math.round(100 * (rt.used || 0) / rt.arena_bytes);
      bits.push(`arena ${Math.round((rt.used || 0) / 1024)}/${Math.round(rt.arena_bytes / 1024)} KB (${pct}%)`);
      // Compiling a module costs far more than running it, so a high-water
      // mark well above the resting figure is normal and worth showing.
      if (rt.peak) bits.push(`peak ${Math.round(rt.peak / 1024)} KB`);
      if (rt.hook_kills) bits.push(`${rt.hook_kills} hook kills`);
      if (rt.slice_overruns) bits.push(`${rt.slice_overruns} slow slices`);
    }
    setStatus(bits.join(' · '));
  }

  /* ---- errors ----------------------------------------------------------- */
  function pushError(script, text) {
    const line = `${new Date().toISOString().slice(11, 19)}  ${script}: ${text}`;
    if (errors[errors.length - 1] === line) return;   // do not repeat a steady error
    errors.push(line);
    if (errors.length > 100) errors.shift();
    el.errs.textContent = errors.join('\n');
    el.errs.scrollTop = el.errs.scrollHeight;
  }

  /* ---- load / save ------------------------------------------------------ */
  async function load(name) {
    if (!name) return;
    if (dirty && !confirm(`Discard unsaved changes to ${current}?`)) return;
    setStatus('loading…');
    try {
      const { source, tier } = await io.source(name);
      el.src.value = source;
      current = name;
      lastLoadedTier = tier;
      dirty = false;
      el.tier.textContent = tier;
      el.tier.dataset.tier = tier;
      setStatus(`loaded ${source.length} B from ${tier}`);
    } catch (e) {
      // A name with no source at all is the normal case for a NEW script, so
      // say so rather than treating it as a failure.
      if (e.code === 'not_found') {
        el.src.value = ''; current = name; dirty = false;
        el.tier.textContent = 'new'; el.tier.dataset.tier = 'new';
        setStatus('no source yet — type some and press Draft');
      } else {
        setStatus(e.message, 'err');
      }
    }
  }

  function nameInPlay() {
    const n = (current || el.sel.value || '').trim();
    if (!n) { setStatus('pick or type a script name', 'err'); return null; }
    return n;
  }

  async function doDraft(run) {
    const n = nameInPlay(); if (!n) return;
    setStatus('uploading draft…');
    try {
      const r = await io.draft(n, el.src.value, {
        run,
        onProgress: (i, total) => setStatus(`uploading draft ${i}/${total}…`),
      });
      dirty = false;
      current = n;
      el.tier.textContent = 'draft'; el.tier.dataset.tier = 'draft';
      setStatus(`draft ${r.bytes} B in RAM · flash untouched${run ? ' · running' : ''}`, 'ok');
      refresh();
    } catch (e) { setStatus(`draft: ${e.message}`, 'err'); pushError(n, e.message); }
  }

  async function simple(op, label) {
    const n = nameInPlay(); if (!n) return;
    try {
      await api.call(op, { script: n });
      setStatus(`${label} ok`, 'ok');
      refresh();
    } catch (e) { setStatus(`${label}: ${e.message}`, 'err'); pushError(n, e.message); }
  }

  /* ---- REPL ------------------------------------------------------------- */
  async function runEval() {
    const n = nameInPlay(); if (!n) return;
    const chunk = el.evalIn.value.trim();
    if (!chunk) return;
    try {
      // A Lua error is a SUCCESSFUL reply carrying ok:false -- the op worked,
      // the chunk did not. Only a transport or dispatch failure rejects.
      const r = await api.call('script.eval', { script: n, chunk });
      const mark = r.ok ? ' ' : '!';
      el.evalOut.textContent = `> ${chunk}\n${mark} ${r.value}\n` + el.evalOut.textContent;
    } catch (e) {
      el.evalOut.textContent = `> ${chunk}\n! ${e.message}\n` + el.evalOut.textContent;
    }
    el.evalIn.value = '';
  }

  /* ---- wiring ----------------------------------------------------------- */
  el.src.addEventListener('input', () => {
    dirty = true;
    el.tier.textContent = lastLoadedTier ? `${lastLoadedTier} (edited)` : 'edited';
    el.tier.dataset.tier = 'edited';
  });
  el.sel.onchange = () => load(el.sel.value);
  $('s-new').onclick = () => {
    const n = prompt('New script name (letters, digits, _ and - only):');
    if (!n) return;
    const o = document.createElement('option');
    o.value = n; o.textContent = n;
    el.sel.appendChild(o); el.sel.value = n;
    current = n; el.src.value = ''; dirty = false;
    el.tier.textContent = 'new'; el.tier.dataset.tier = 'new';
    setStatus('new script — type some Lua and press Draft');
  };
  $('s-reload').onclick = () => load(nameInPlay());
  $('s-draft').onclick = () => doDraft(true);
  $('s-draft-only').onclick = () => doDraft(false);
  $('s-stage').onclick = () => simple('script.stage', 'stage');
  $('s-commit').onclick = () => simple('script.commit', 'commit');
  $('s-revert').onclick = async () => { await simple('script.revert', 'revert'); load(nameInPlay()); };
  $('s-start').onclick = () => simple('script.start', 'start');
  $('s-stop').onclick = () => simple('script.stop', 'stop');
  $('s-errors-clear').onclick = () => { errors.length = 0; el.errs.textContent = ''; };
  el.evalIn.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); runEval(); }
  });
  // Tab should indent, not leave the editor -- this is the one place in the
  // app where losing focus mid-thought is actively annoying.
  el.src.addEventListener('keydown', (e) => {
    if (e.key !== 'Tab') return;
    e.preventDefault();
    const s = el.src.selectionStart, t = el.src.selectionEnd;
    el.src.value = el.src.value.slice(0, s) + '  ' + el.src.value.slice(t);
    el.src.selectionStart = el.src.selectionEnd = s + 2;
    dirty = true;
  });

  api.on('script.error', d => {
    pushError(d.script || '?', d.error || JSON.stringify(d));
    refresh();
  });
  api.on('_open', refresh);

  return { refresh, load };
}
