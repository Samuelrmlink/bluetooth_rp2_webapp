// Devices and their plugins: discovered from telemetry, loaded only when
// looked at, and taken apart again when they are not.
//
//   discovered   a running script publishes profile.<x>.telemetry; the
//                registry names a plugin and the device's windows are
//                REGISTERED -- they appear in the window picker. Nothing is
//                fetched yet.
//   mounted      one of those windows comes on screen: the plugin module is
//                imported (once per type; ES modules are cached by the
//                browser), its stylesheet linked, the window built.
//   unmounted    the window is parked or closed: its DOM is emptied, every
//                subscription it made is dropped through its bus scope, and
//                the stylesheet goes with the type's last mounted window.
//   gone         no telemetry for GONE_MS: windows on screen say so; once
//                none are, the device's windows are unregistered.
//
// "Unloading" cannot mean evicting the module -- no browser does that -- so
// it means nothing of the plugin remains wired to the page: no DOM, no
// listener, no stylesheet. That is the part that costs anything.

import { pluginFor, deviceKey, DEFAULT_WINDOWS } from './registry.js';
import { BUILTIN_KINDS } from './windows.js';

const GONE_MS = 30000;

class LazyWindow {
  constructor(host, dev, spec, el) {
    this.host = host;
    this.dev = dev;
    this.spec = spec;
    this.el = el;
    this.inst = null;
    this._gen = 0;
    this._onSample = (s) => this.inst?.update?.(s);
  }

  async onShow() {
    const gen = ++this._gen;
    const plugin = await this.host.load(this.dev.plugin);
    if (gen !== this._gen || this.inst) return;       // hidden again while loading
    this.scope = this.host.app.bus.owner();
    const Kind = plugin.windows?.[this.spec.kind] || BUILTIN_KINDS[this.spec.kind];
    this.el.replaceChildren();
    this.inst = new Kind(this.el, {
      ws: this.host.app.ws,
      bus: this.scope,
      device: this.dev,
      plugin,
      log: (s, t) => this.host.app.log(s, t),
    });
    this.host.cssAcquire(this.dev.plugin, plugin);
    this.dev.listeners.add(this._onSample);
    if (this.dev.seen) this.inst.update?.(this.dev.sample);
  }

  onHide() {
    this._gen++;
    if (!this.inst) return;
    this.inst.unmount?.();
    this.dev.listeners.delete(this._onSample);
    this.scope.dispose();
    this.el.replaceChildren();
    this.inst = null;
    this.host.cssRelease(this.dev.plugin);
  }

  render(t) { this.inst?.render?.(t); }
  get mounted() { return !!this.inst; }
}

export class DeviceHost {
  constructor(app) {
    this.app = app;
    this.devices = new Map();          // key -> device
    this._modules = new Map();         // plugin id -> Promise<module>
    this._css = new Map();             // plugin id -> { link, count }
    this.scope = app.bus.owner();
    this.scope.on('profile.*', (d, topic) => this._onEvent(topic, d));
    setInterval(() => this._sweep(), 5000);
  }

  // A plugin's module; the generic plugin has none. Imported once.
  load(entry) {
    if (!entry.module) return Promise.resolve(entry);
    if (!this._modules.has(entry.id)) {
      this._modules.set(entry.id, import(new URL(entry.module, import.meta.url).href)
        .then((m) => ({ ...entry, ...m }))
        .catch((e) => {
          this._modules.delete(entry.id);   // let a later open retry
          this.app.log('!', `plugin ${entry.id}: ${e.message}`);
          return entry;                     // fall back to the built-in windows
        }));
    }
    return this._modules.get(entry.id);
  }

  cssAcquire(entry, plugin) {
    if (!plugin.css) return;
    let c = this._css.get(entry.id);
    if (!c) {
      const link = document.createElement('link');
      link.rel = 'stylesheet';
      link.href = new URL(plugin.css, new URL(entry.module, import.meta.url)).href;
      link.dataset.plugin = entry.id;
      document.head.appendChild(link);
      c = { link, count: 0 };
      this._css.set(entry.id, c);
    }
    c.count++;
  }

  cssRelease(entry) {
    const c = this._css.get(entry.id);
    if (!c) return;
    if (--c.count <= 0) {
      c.link.remove();
      this._css.delete(entry.id);
    }
  }

  _onEvent(topic, d) {
    if (!topic.endsWith('.telemetry') || !d || typeof d !== 'object') return;
    const key = deviceKey(topic, d);
    let dev = this.devices.get(key);
    if (!dev) {
      const plugin = pluginFor(topic, d);
      const name = plugin.name(d, topic);
      dev = {
        key, plugin, topic, addr: d.addr || '', sample: {}, seen: 0, gone: false,
        name: this._uniqueName(name, d.addr), listeners: new Set(), windows: new Map(),
      };
      this.devices.set(key, dev);
      this._register(dev);
    }
    dev.sample = { ...dev.sample, ...d };
    dev.seen = Date.now();
    dev.gone = false;
    for (const fn of dev.listeners) fn(dev.sample);
  }

  // Two C1000s must not share a picker group: add the address's tail.
  _uniqueName(name, addr) {
    const clash = [...this.devices.values()].some((x) => x.name === name);
    return clash && addr ? `${name} (${addr.slice(-5)})` : name;
  }

  _register(dev) {
    const specs = dev.plugin.windows || DEFAULT_WINDOWS;
    for (const spec of specs) {
      const id = `${dev.plugin.id}.${spec.kind}@${dev.key}`;
      const win = this.app.addWindow(id, {
        // short is what a picker shows CLOSED: with two devices, "Controls"
        // alone does not say whose -- so it carries the device name too.
        title: `${dev.name} · ${spec.title}`, group: dev.name, short: `${dev.name} · ${spec.title}`,
      }, (el) => new LazyWindow(this, dev, spec, el));
      dev.windows.set(id, win);
    }
    this.app.log('device', `${dev.name} discovered — open it from any window's picker`);
  }

  _sweep() {
    const now = Date.now();
    for (const [key, dev] of this.devices) {
      if (!dev.seen || now - dev.seen < GONE_MS) continue;
      dev.gone = true;
      const onScreen = [...dev.windows.values()].some((w) => w.mounted);
      if (onScreen) continue;          // say "not heard for N s" until closed
      for (const id of dev.windows.keys()) this.app.removeWindow(id);
      this.devices.delete(key);
      this.app.log('device', `${dev.name} went quiet — its windows were removed`);
    }
  }
}
