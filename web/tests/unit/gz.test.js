import { test } from 'node:test';
import assert from 'node:assert/strict';
import { gunzipSync } from 'node:zlib';
import { isGz, plainName, imageKind, imageType, isWebAsset, canCompress, shadowInfo, gzipText, gunzipText } from '../../js/gz.js';
import { languageOf } from '../../js/editor/highlight.js';
import { scriptNameOf } from '../../js/editor/status.js';

test('names: .gz is appended, the contents keep their own type', () => {
  assert.equal(isGz('lua/run.lua.gz'), true);
  assert.equal(plainName('js/app.js.gz'), 'js/app.js');
  assert.equal(languageOf('lua/run.lua.gz'), 'lua');
  assert.equal(languageOf('cfg/x.json'), 'json');
  assert.equal(scriptNameOf('lua/anker.lua.gz'), 'anker');
  assert.equal(scriptNameOf('lua/profiles/x.lua.gz'), null);
});

test('images: bitmaps shown, SVG shown or edited, .svg.gz too', () => {
  assert.equal(imageKind('img/a.PNG'), 'bitmap');
  assert.equal(imageKind('ss/photo.jpeg'), 'bitmap');
  assert.equal(imageKind('favicon.ico'), 'bitmap');
  assert.equal(imageKind('icons.svg.gz'), 'svg');
  assert.equal(imageKind('js/app.js.gz'), null);
  assert.equal(imageType('a.svg.gz'), 'image/svg+xml');
  assert.equal(imageType('a.webp'), 'image/webp');
});

test('web assets get a .bak; the device\'s own folders do not', () => {
  assert.equal(isWebAsset('js/app.js.gz'), true);
  assert.equal(isWebAsset('css/app.css.gz'), true);
  assert.equal(isWebAsset('index.html.gz'), true);
  assert.equal(isWebAsset('pinecil/index.html'), true);
  assert.equal(isWebAsset('lua/run.lua'), false);
  assert.equal(isWebAsset('cfg/wifi.json'), false);
});

test('what may be stored compressed', () => {
  assert.equal(canCompress('lua/run.lua'), true);
  assert.equal(canCompress('js/x.js'), true);
  assert.equal(canCompress('lua/run.lua.gz'), false);
  assert.equal(canCompress('cfg/wifi.json'), false);
  assert.equal(canCompress('staged/x.lua'), false);
  assert.equal(canCompress('ss/a.jpg'), false);
  assert.equal(canCompress('pex/anker.m33.pex'), false);
  assert.equal(canCompress('js/x.js.gz.bak'), false);
  assert.equal(canCompress('css/app.css'), true);
  assert.equal(canCompress('index.html'), true);
  assert.equal(canCompress('notes.txt'), false);
  assert.equal(canCompress('src/main.c'), false);
  assert.equal(canCompress('lua/profiles/anker/run.lua'), false);
  assert.equal(canCompress('pex/anker.m33.pex.gz'), false);
});

test('plain wins: the .gz twin is shadowed', () => {
  assert.deepEqual(shadowInfo('lua/x.lua.gz', ['x.lua', 'x.lua.gz']), { shadowed: true, by: 'x.lua' });
  assert.deepEqual(shadowInfo('lua/x.lua', ['x.lua', 'x.lua.gz']), { shadowed: false, by: 'x.lua.gz' });
  assert.equal(shadowInfo('lua/x.lua', ['x.lua']), null);
});

test('gzipText makes real gzip, and gunzipText reads it back', async () => {
  const text = 'local x = 1\n'.repeat(500) + 'ü';
  const blob = await gzipText(text);
  const bytes = new Uint8Array(await blob.arrayBuffer());
  assert.deepEqual([...bytes.subarray(0, 2)], [0x1f, 0x8b]);
  assert.ok(bytes.length < text.length / 10);
  assert.equal(gunzipSync(bytes).toString('utf8'), text);
  assert.equal(await gunzipText(bytes), text);
});
