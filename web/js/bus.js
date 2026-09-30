// Device events, with subscriptions that can be dropped all at once.
//
// The gateway broadcasts every event to every client -- there is no
// server-side subscription -- so filtering happens here. wsapi.js already
// delivers by exact topic and to '*'; this adds prefix topics
// ('profile.anker.*') and, more importantly, OWNERSHIP: every window and
// every device plugin subscribes through its own handle, and disposing the
// handle drops everything it subscribed to. That is what makes unmounting a
// plugin real rather than hopeful -- a forgotten listener would keep a
// closed window's code wired to the event stream for the life of the page.

export class Bus {
  constructor(ws) {
    this._ws = ws;
  }

  // A subscription scope. Dispose it when its owner goes away.
  owner() {
    const offs = [];
    const ws = this._ws;
    const scope = {
      on(topic, fn) {
        let off;
        if (topic.endsWith('*') && topic !== '*') {
          const prefix = topic.slice(0, -1);
          off = ws.on('*', (m) => { if (m.evt.startsWith(prefix)) fn(m.data, m.evt); });
        } else {
          off = ws.on(topic, (d) => fn(d, topic));
        }
        offs.push(off);
        return off;
      },
      dispose() {
        while (offs.length) offs.pop()();
      },
      get size() { return offs.length; },
    };
    return scope;
  }
}
