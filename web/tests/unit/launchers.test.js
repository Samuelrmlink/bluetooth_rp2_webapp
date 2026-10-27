import { test } from 'node:test';
import assert from 'node:assert/strict';

const {
  appNames, cleanLabel, parseLabel, collate, validHref, normalizeLauncher,
  createRegistry, loadFolderLaunchers, hrefFor,
} = await import('../../js/launchers.js');

test('launchers: folder names only, safe, in order', () => {
  assert.deepEqual(appNames([
    { name: 'printer', dir: true }, { name: 'readme.txt', size: 3 }, { name: 'a_b', dir: true },
    { name: '../x', dir: true }, { name: 'sp ace', dir: true }, { name: 'my-app', dir: true }, null,
  ]), ['a_b', 'my-app', 'printer']);
  assert.deepEqual(appNames(undefined), []);
});

test('launchers: label, the title alias, trimming, and the cap', () => {
  assert.equal(parseLabel('{"label":" Printer "}', 'printer'), 'Printer');
  assert.equal(parseLabel('{"title":"Gauge"}', 'gauge'), 'Gauge');
  assert.equal(parseLabel('{"label":"Printer","title":"Other"}', 'printer'), 'Printer');
  assert.equal(parseLabel('{"label":"  "}', 'printer'), 'printer');
  assert.equal(parseLabel('{"href":"https://evil.example"}', 'printer'), 'printer');
  assert.equal(parseLabel('{"label":"bad\\u0000name"}', 'x'), 'badname');
  assert.equal(cleanLabel('x'.repeat(50), 'id').length, 40);
  assert.equal(parseLabel('{', 'x'), null);
  assert.equal(parseLabel('[]', 'x'), null);
  assert.equal(parseLabel('null', 'x'), null);
});

test('launchers: the link is the folder, and a shared name stays two buttons', () => {
  const one = collate([], [{ id: 'printer', label: 'Printer' }]);
  assert.deepEqual(one, [{
    id: 'printer', label: 'Printer', href: '/sd/apps/printer/', onClick: null, volume: 'sd',
  }]);
  const both = collate(
    [{ id: 'printer', label: 'Printer' }],
    [{ id: 'printer', label: 'Printer' }],
  );
  assert.equal(both[0].id, 'printer');
  assert.equal(both[0].label, 'Printer (device)');
  assert.equal(both[0].href, '/apps/printer/');
  assert.equal(both[1].id, 'sd-printer');
  assert.equal(both[1].label, 'Printer (card)');
  assert.equal(both[1].href, '/sd/apps/printer/');
  const named = collate(
    [{ id: 'printer', label: 'Bench' }],
    [{ id: 'printer', label: 'Printer' }],
  );
  assert.equal(named[0].label, 'Bench');
  assert.equal(named[1].label, 'Printer');
  assert.equal(named[1].id, 'sd-printer');
  assert.equal(hrefFor('lfs', 'a b'), '/apps/a%20b/');
});

test('launchers: href is a device path; onClick is only the fallback', () => {
  assert.equal(validHref('/sd/apps/printer/'), true);
  assert.equal(validHref('/apps/printer/?x=1'), true);
  assert.equal(validHref('//evil.example'), false);
  assert.equal(validHref('https://evil.example'), false);
  assert.equal(validHref('printer'), false);
  assert.equal(validHref('/ok\n'), false);
  const link = normalizeLauncher({ id: 'printer', label: 'Printer', href: '/sd/apps/printer/', onClick() {} });
  assert.equal(link.ok, true);
  assert.equal(link.launcher.href, '/sd/apps/printer/');
  assert.equal(link.launcher.onClick, null);
  const click = normalizeLauncher({ id: 'tool', onClick() { return 1; } });
  assert.equal(click.ok, true);
  assert.equal(click.launcher.href, null);
  assert.equal(typeof click.launcher.onClick, 'function');
  assert.equal(click.launcher.label, 'tool');
  assert.deepEqual(normalizeLauncher({ id: 'printer', href: 'https://evil.example' }), {
    ok: false, error: 'href must be a path on this device',
  });
  assert.equal(normalizeLauncher({ id: '../x', href: '/a' }).ok, false);
  assert.equal(normalizeLauncher({ id: 'printer' }).ok, false);
});

test('launchers: a second registration of the same id replaces the first', () => {
  const reg = createRegistry();
  assert.equal(reg.set({ id: 'printer', label: 'Printer', href: '/sd/apps/printer/', onClick: null, volume: 'api' }), false);
  assert.equal(reg.set({ id: 'printer', label: 'Labels', href: '/sd/apps/printer/', onClick: null, volume: 'api' }), true);
  assert.equal(reg.get('printer').label, 'Labels');
  assert.deepEqual(reg.replaceVolume('sd', [
    { id: 'printer', label: 'Printer', href: '/sd/apps/printer/', onClick: null, volume: 'sd' },
  ]), ['printer']);
  assert.equal(reg.get('printer').volume, 'sd');
  assert.equal(reg.remove('printer'), true);
  assert.equal(reg.has('printer'), false);
});

function files(names) {
  return { ok: true, files: names.map((name) => ({ name, dir: true })) };
}

test('launchers: manifests on both trees, gzip fallback, a missing file, bad JSON', async () => {
  const logs = [];
  const reads = {
    'apps/gauge/app.json': { ok: true, text: '{"title":"Gauge"}' },
    'sd/apps/printer/app.json': { ok: false, status: 404, error: 'not found' },
    'sd/apps/printer/app.json.gz': { ok: true, text: '{"label":"Printer","href":"https://evil.example"}' },
    'sd/apps/notes/app.json': { ok: false, status: 404, error: 'not found' },
    'sd/apps/notes/app.json.gz': { ok: false, status: 404, error: 'not found' },
    'sd/apps/broken/app.json': { ok: true, text: '{' },
  };
  const found = await loadFolderLaunchers({
    sd: true,
    log: (src, text) => logs.push([src, text]),
    listDir: async (dir) => {
      if (dir === 'apps') return files(['gauge', 'sp ace']);
      if (dir === 'sd/apps') return files(['printer', 'notes', 'broken', '../x']);
      throw new Error(dir);
    },
    readText: async (path) => reads[path] || { ok: false, status: 404, error: 'not found' },
  });
  assert.deepEqual(found.lfs, [{
    id: 'gauge', label: 'Gauge', href: '/apps/gauge/', onClick: null, volume: 'lfs',
  }]);
  assert.deepEqual(found.sd, [{
    id: 'printer', label: 'Printer', href: '/sd/apps/printer/', onClick: null, volume: 'sd',
  }]);
  assert.deepEqual(logs, [['!', 'launcher broken: bad app.json']]);
});

test('launchers: a missing apps directory is nothing; a failed listing is kept', async () => {
  const quiet = await loadFolderLaunchers({
    sd: false,
    listDir: async () => ({ ok: false, status: 404, error: 'not found' }),
    readText: async () => { throw new Error('must not read'); },
  });
  assert.deepEqual(quiet, { lfs: [], sd: [] });
  const logs = [];
  const failed = await loadFolderLaunchers({
    sd: true,
    log: (src, text) => logs.push([src, text]),
    listDir: async (dir) => dir === 'apps'
      ? { ok: false, status: 503, error: 'busy' }
      : { ok: true, files: [{ name: 'printer', dir: true }] },
    readText: async () => ({ ok: true, text: '{"label":"Printer"}' }),
  });
  assert.equal(failed.lfs, null);
  assert.equal(failed.sd[0].id, 'printer');
  assert.match(logs[0][1], /apps: busy/);
});
