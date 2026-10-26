import { test } from 'node:test';
import assert from 'node:assert/strict';

function domStub() {
  function node(tag) {
    const el = {
      tag,
      className: '',
      textContent: '',
      children: [],
      dataset: {},
      style: {},
      draggable: false,
      tabIndex: 0,
      setAttribute(k, v) { this[k] = v; },
      classList: {
        add(c) { if (!el.className.split(/\s+/).includes(c)) el.className = `${el.className} ${c}`.trim(); },
      },
      append(...kids) { this.children.push(...kids); },
      appendChild(kid) { this.children.push(kid); return kid; },
      replaceChildren(...kids) {
        const flat = [];
        for (const kid of kids) {
          if (kid?.fragment) flat.push(...kid.children);
          else flat.push(kid);
        }
        this.children = flat;
      },
      querySelector() { return null; },
    };
    return el;
  }
  globalThis.document = {
    createElement: node,
    createDocumentFragment() {
      return {
        fragment: true,
        children: [],
        appendChild(kid) { this.children.push(kid); return kid; },
      };
    },
  };
}

test('LittleFS listings spin, and the SD card does not', async () => {
  globalThis.location = { search: '', hostname: 'localhost', port: '', protocol: 'http:' };
  const { listsWithSpinner, FileTree } = await import('../../js/editor/filetree.js');
  assert.equal(listsWithSpinner('lua'), true);
  assert.equal(listsWithSpinner(''), true);
  assert.equal(listsWithSpinner('cfg/bt.json'), true);
  assert.equal(listsWithSpinner('sd'), false);
  assert.equal(listsWithSpinner('sd/projects/blink/build'), false);

  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  globalThis.fetch = async (url) => {
    await gate;
    const sd = String(url).includes('/api/sd/');
    return {
      ok: true,
      status: 200,
      json: async () => ({
        ok: true,
        block: 4096,
        files: sd ? [{ name: 'a.txt', size: 3 }] : [{ name: 'a.txt', size: 3, alloc: 0 }],
      }),
    };
  };

  domStub();
  const host = document.createElement('div');
  const tree = new FileTree(host, { onOpen() {}, mark() { return {}; }, onError() {} });
  const lua = { path: 'lua', name: 'lua', dir: true, open: true, children: null };
  const pending = tree.load(lua);
  assert.equal(lua.loading, true);
  assert.equal(tree._dirty, true);

  const card = { path: 'sd/logs', name: 'logs', dir: true, open: true, children: null };
  const cardPending = tree.load(card);
  assert.equal(card.loading, undefined);

  release();
  await pending;
  await cardPending;
  assert.equal(lua.loading, false);
  assert.equal(lua.children[0].name, 'a.txt');
  assert.equal(card.loading, undefined);
  assert.equal(card.children[0].name, 'a.txt');

  tree.root.children = [lua, card];
  lua.loading = true;
  lua.open = true;
  lua.children = [];
  card.loading = false;
  card.open = true;
  tree._dirty = true;
  tree.render();
  const rows = host.children;
  assert.match(rows[0].className, /is-loading/);
  assert.equal(rows[0].children[0].textContent, '');
  assert.doesNotMatch(rows[1].className, /is-loading/);
  assert.equal(rows[1].children[0].textContent, '▾');
});
