--[[
The Anker driver, as a module so more than one device can be driven at once.

lua/anker.lua and any sibling launcher are two lines each; the work is here,
compiled ONCE however many devices are being watched. That matters on a
128 KB arena: two copies of this would not fit, and two launchers sharing it
comfortably do.

Everything below is described in the launcher's own comment. In short: the
advertisement decides the model and which cipher to try first, each stage
loads exactly one module and unloads it before the next compiles, and the
per-model schema is the last thing standing.
]]

-- link and schema are needed for the whole session, so they are held here.
-- adv is NOT: it is required inside run() instead, so that each launcher
-- holds its own reference and can let go once it has identified its device.
-- Held as a module upvalue it would be pinned forever, and `unload` would
-- free nothing -- observed as "released profiles.anker.adv, heap 62144 ->
-- 62144 B", which is the shape of an unload that did not.
local link_m = require 'profiles.anker.link'
local schema = require 'profiles.anker.schema'
local control = require 'profiles.anker.control'
local setter = require 'profiles.anker.setter'

local R = {}

local function load_cfg()
  local pk = sys.secret('anker', 'private_key')
  if not pk then return nil, 'cfg/anker.json needs private_key' end
  local key = bytes.unhex(pk)
  if not key or #key ~= 32 then return nil, 'private_key must be 64 hex characters' end
  return {
    private_key   = key,
    user_id       = sys.secret('anker', 'user_id'),
    tz            = sys.secret('anker', 'tz') or 'UTC0',
    tz_offset_sec = tonumber(sys.secret('anker', 'tz_offset_sec') or '0') or 0,
    client_id     = sys.secret('anker', 'client_id') or 'btgw-0000',
    region        = sys.secret('anker', 'region') or 'US',
  }
end

local function drop(name, alsonil)
  if alsonil then alsonil() end
  local before = sys.collect()
  local had, after = sys.unload(name)
  log.info(('anker: released %s (%s), heap %d -> %d B')
           :format(name, had and 'unloaded' or 'was not cached', before, after))
end

-- Device-independent cleanup for one session of one launcher: whatever ends
-- it -- a return, an error, a stop, a lost link -- its claim, its handshake
-- lock and its entry in control.devices go, so the other launcher can take
-- the device and nothing routes writes to a dead session. A to-be-closed
-- variable because a coroutine killed mid-wait is closed with
-- lua_closethread (lrt.c release_runtime), which runs these and nothing else.
local function session_guard(who, state)
  return setmetatable({}, { __close = function()
    for a, w in pairs(control.claims) do
      if w == who then control.claims[a] = nil end
    end
    if control.handshake and control.handshake.who == who then control.handshake = nil end
    if state.addr and control.devices[state.addr] and control.devices[state.addr].who == who then
      control.devices[state.addr] = nil
    end
    -- A session that failed half way (no cipher, discovery timed out) can
    -- leave the link up; the next attempt starts from a clean connect.
    if state.addr then bt.disconnect(state.addr) end
  end })
end

-- Wait for the other launcher's handshake to finish, then hold the lock.
local HANDSHAKE_MS = 45000
local function handshake_lock(who)
  while true do
    local h = control.handshake
    if not h or h.who == who or sys.ms() > h.until_ms then break end
    sys.sleep(250)
  end
  control.handshake = { who = who, until_ms = sys.ms() + HANDSHAKE_MS }
end
local function handshake_unlock(who)
  if control.handshake and control.handshake.who == who then control.handshake = nil end
end

local function norm(a) return (tostring(a or ''):upper():gsub('[^0-9A-F]', '')) end

-- anker.set routes to whichever launcher holds the device. One handler for
-- every launcher: registering replaces, and a handler that only knew its own
-- device refused writes to the other one. Ops outlive the script that
-- registered them, so this must not hold anything of a session -- it reads
-- the shared table on every call.
local function dispatch_set(p)
  p = p or {}
  local want = norm(p.device)
  for a, d in pairs(control.devices) do
    if norm(a) == want and d.set then return d.set(p.field, p.value) end
  end
  return { error = 'not_this_device', message = 'no Anker launcher holds ' .. tostring(p.device) }
end

function R.run(target_override, who)
  who = who or 'anker'
  local state = {}
  local _guard <close> = session_guard(who, state)
  local adv = require 'profiles.anker.adv'
  local cfg, err = load_cfg()
  if not cfg then log.error('anker: ' .. err); return 'fatal' end
  -- The launcher's pin, if it has one: lua/anker.lua passes cfg "target",
  -- lua/anker2.lua "target2". An address or a productType (e.g. "B105").
  -- Taken from the cfg here once, it pinned EVERY launcher to the first
  -- one's device, and the second waited forever for a device already held.
  cfg.target = target_override

  -- ---------------------------------------------------------------------------
  -- 1. Identify from the advertisement
  -- ---------------------------------------------------------------------------
  -- nil target means "any"; otherwise match an address or a productType.
  local function wanted(d, got)
    if not cfg.target then return true end
    local t = cfg.target:upper()
    return d.addr:upper() == t or got.product_type == t
  end

  log.info(('%s: looking for %s'):format(who, cfg.target or 'an Anker device no other launcher holds'))
  local addr, a
  -- Scan hard for a while, then in bursts: a launcher whose device is not
  -- around (switched off, or never here) must not hold the radio in a scan
  -- indefinitely -- a running scan keeps le_radio_busy() true for WiFi too.
  local hunt_until = sys.ms() + 30000
  repeat
    -- Make the radio actually look. bt.devices() is the scan TABLE, not a
    -- scan: it returns whatever was last seen, so polling it without
    -- scanning spins here forever and says nothing. That is not
    -- hypothetical -- this loop sat at "looking for an Anker device" with
    -- both chargers advertising a metre away, because an earlier scan.stop
    -- had left the radio idle. Asking every pass is harmless: bt.scan()
    -- returns immediately when a scan is already running, rather than
    -- restarting one and losing the window an advertisement arrives in.
    bt.scan(true)
    for _, d in ipairs(bt.devices()) do
      if not addr and not control.claims[d.addr] then
        local got = adv.identify(d)
        -- Checked and claimed with no yield in between: atomic.
        if got and wanted(d, got) then
          addr, a = d.addr, got
          control.claims[addr] = who
        end
      end
    end
    if not addr then
      if sys.ms() < hunt_until then
        sys.sleep(500)
      else
        sys.sleep(4000)          -- scan for ~4 s ...
        bt.scan(false)
        sys.sleep(11000)         -- ... then leave the radio alone for 11
      end
    end
  until addr
  state.addr = addr

  -- Stop scanning before connecting. The radio is shared with WiFi and with
  -- the other launcher, and a scan left running is the thing that makes
  -- le_radio_busy() true for no reason.
  bt.scan(false)

  -- One handshake at a time, from here to the end of authentication.
  handshake_lock(who)

  log.info(('anker: %s is productType %s, sku %s, v%d, %s, capability 0x%02x (%s)')
           :format(addr, a.product_type, tostring(a.sku), a.version,
                   a.bound and 'bound' or 'unbound', a.capability,
                   a.gcm_hint and 'gcm hint' or 'no gcm hint'))

  -- ---------------------------------------------------------------------------
  -- 2. Connect and discover
  -- ---------------------------------------------------------------------------
  local function is_connected()
    for _, d in ipairs(bt.devices()) do
      if d.addr == addr and d.connected then return true end
    end
    return false
  end

  -- Two phases. bt.connect only reports that the attempt was STARTED, so a
  -- single loop that gives up when the device is "not connected" fires before
  -- the link has had any chance. Wait FOR the link, then watch for it dropping.
  local function bring_up()
    if not is_connected() then
      if not bt.connect(addr) then return false, 'connect refused' end
      local dl = sys.ms() + 12000
      while not is_connected() and sys.ms() < dl do sys.sleep(250) end
      if not is_connected() then return false, 'link never came up' end
    end
    local dl = sys.ms() + 20000
    repeat
      if bt.find(addr, link_m.SVC, link_m.CMD) then return true end
      if not is_connected() then return false, 'link dropped during discovery' end
      sys.sleep(250)
    until sys.ms() > dl
    return false, 'discovery never finished'
  end

  local ok, why
  for attempt = 1, 4 do
    ok, why = bring_up()
    if ok then break end
    log.warn(('anker: attempt %d failed (%s)'):format(attempt, tostring(why)))
    sys.sleep(1500)
  end
  if not ok then log.error('anker: giving up: ' .. tostring(why)); return end
  log.info('anker: link up and discovered')

  -- adv has done its whole job. Dropping our reference first is what makes
  -- the unload mean anything; if a second launcher is still identifying it
  -- keeps its own, and the collect simply finds the module still reachable.
  adv = nil
  drop('profiles.anker.adv')

  local link = link_m.open(addr)
  if not bt.subscribe(addr, link.tel, 'notify') then
    log.error('anker: could not subscribe to the telemetry characteristic'); return
  end

  -- ---------------------------------------------------------------------------
  -- 3. Negotiate a session, one cipher module at a time
  -- ---------------------------------------------------------------------------
  --
  -- The capability bit orders the attempts; it does not decide. Both of the
  -- devices on this bench set it and only one of them speaks GCM, because the
  -- app also consults a cloud value we cannot read. So: try, then check.
  local order = a.gcm_hint and { 'neg_gcm', 'neg_cbc' } or { 'neg_cbc', 'neg_gcm' }

  -- A cipher is right if a request under it comes back readable. Sending
  -- getAllDeviceInfo is the cheapest thing that is meaningful on both families;
  -- a wrong key yields high-entropy noise whose first byte is almost never a
  -- valid status and whose second is almost never a TLV tag.
  local function probe(l)
    local dec = l:secure(0x0F, 0x0040,
      tlv.build{ { tag = 0xa1, enc = 'bytes', value = '\x21', type_byte = false } },
      0x0840, 4000)
    if not dec or not dec.body or #dec.body < 2 then return false end
    local tag = dec.body:byte(1)
    return (tag >= 0xa0 and tag <= 0xff) or tag == 0x00
  end

  local negotiated, bind_burst
  for _, name in ipairs(order) do
    local mod = 'profiles.anker.' .. name
    local neg = require(mod)
    local good, nerr = neg.negotiate(link, cfg, probe)
    if good then
      negotiated = neg.name
      log.info(('anker: negotiated over %s'):format(negotiated))
      -- 0x0027 authorises the session and is needed by BOTH families; the
      -- mini-charge burst after it is not, so it waits for the schema.
      if neg.authenticate and cfg.user_id then
        local aok, aerr = neg.authenticate(link, cfg)
        if not aok then log.warn('anker: getAuthentication: ' .. tostring(aerr)) end
      elseif not cfg.user_id then
        log.warn('anker: no user_id in cfg; the device may refuse to report')
      end
      -- Whether the bind burst is wanted is a property of the MODEL, which is
      -- not known until the schema loads -- but keeping the whole negotiation
      -- module resident until then costs more arena than compiling the lookup
      -- table has spare. So keep the one closure and drop the rest: unload
      -- frees what is unreachable, and a function we still hold is reachable.
      bind_burst = neg.bind_burst
      neg = nil
      drop(mod)
      break
    end
    log.warn(('anker: %s did not take (%s)'):format(name, tostring(nerr)))
    neg = nil
    drop(mod)
  end
  if not negotiated then log.error('anker: no cipher produced a session'); return end

  -- ---------------------------------------------------------------------------
  -- 4. Resolve the model, then drop the tables
  -- ---------------------------------------------------------------------------
  local product_code, product_name, inferred, region
  do
    local lookup = require 'profiles.anker.lookup'
    product_code, product_name, inferred = lookup.lookup(a.product_type)
    lookup = nil
    drop('profiles.anker.lookup')

    if product_code and a.sku then
      local country = require 'profiles.anker.country'
      region = country.lookup(product_code, a.sku)
      country = nil
      drop('profiles.anker.country')
    end
  end

  if not product_code then
    log.warn(('anker: productType %s is not in the table'):format(a.product_type))
  else
    log.info(('anker: %s is %s%s%s'):format(a.product_type, product_code,
             product_name and (' -- ' .. product_name) or '',
             region and (' [' .. region .. ']') or ''))
    if inferred then log.warn('anker: that mapping is marked inferred, not confirmed') end
  end

  -- ---------------------------------------------------------------------------
  -- 5. Load exactly one schema
  -- ---------------------------------------------------------------------------
  local model, model_mod
  if product_code then
    model_mod = 'profiles.anker.models.' .. product_code
    local got, m = pcall(require, model_mod)
    if got then
      model = m
      log.info(('anker: schema %s loaded%s'):format(product_code,
               m.verified and '' or ' (UNVERIFIED -- field names are guesses)'))
    else
      model_mod = nil
      log.warn(('anker: no schema for %s; reporting raw tags'):format(product_code))
    end
  end

  -- ---------------------------------------------------------------------------
  -- 6. Stream
  -- ---------------------------------------------------------------------------
  local first = true
  local function publish(dec)
    local t
    if model then
      t = schema.decode(model, dec.body, first)
    else
      -- No schema: still useful. Hex is better than nothing and is what the
      -- next schema gets written from.
      t = { raw = bytes.hex(dec.body:sub(1, 128)) }
    end
    if not t then return end
    -- The first report of a session carries its own raw body. Every schema
    -- here is empirical -- there is no tag dictionary in the vendor app, which
    -- has been checked -- so authoring or re-checking one means getting the
    -- plaintext off the device, and without this the only route is a firmware
    -- change, which is the loop this overlay exists to avoid.
    --
    -- Once per session: ~700 characters is cheap once and a leak every five
    -- seconds. It is also what made the JSON encoder's buffer overrun visible,
    -- since this is the field that first pushed an event past 1536 bytes.
    if first then t.body_hex = bytes.hex(dec.body) end
    first = false
    t.addr = addr
    t.product_type = a.product_type
    t.product_code = product_code
    t.model_name = t.model_name or product_name
    t.region = region
    t.cipher = negotiated
    t.serial = t.serial_number or link.serial
    bus.emit('anker.telemetry', t)
    local c = control.devices[addr]
    if c then c.sample, c.at = t, sys.ms() end
  end

  link.on_unsolicited = function(_, f, dec)
    if model and (f.opcode == model.report or (model.also and model.also[f.opcode])) then
      publish(dec)
    end
  end

  local polls = 0
  -- The mini-charge bind burst, now that the model has had its say. Only the
  -- SCP chargers need it; sending it to a power station costs a four-second
  -- timeout per opcode and achieves nothing.
  if model and model.post_auth then
    if bind_burst and cfg.user_id then
      local aok, aerr = bind_burst(link, cfg)
      -- Not fatal: a session without the burst still decodes whatever the
      -- device volunteers; it may simply volunteer nothing.
      if not aok then log.warn('anker: bind burst: ' .. tostring(aerr)) end
    else
      log.warn('anker: this model needs the bind burst and cfg has no user_id')
    end
  end
  bind_burst = nil
  handshake_unlock(who)
  log.info(('%s: streaming from %s (%s), live heap %d B')
           :format(who, addr, tostring(product_code), sys.collect()))

  --[[
  anker.set {device, field, value}: change a setting on the device.

  QUEUED, not sent from the handler: an op handler may not yield (see
  src/lua/lbind.c) and a setter must wait for its reply, so the handler only
  validates and queues, and the poll loop below sends what is queued between
  reports -- then polls at once, so the new value shows up in telemetry
  within a second rather than a poll period later. The web UI shows the
  control as pending until that report confirms it.

  Only fields in the model's `setters` table can be written: their payloads
  were captured from the official app. Anything else is refused by name.

  One launcher's handler per op name: registering again replaces it, so with
  two Anker devices on two launchers the last one's handler wins and the
  other device's writes are refused with a clear error rather than sent to
  the wrong unit. One C1000, as on the bench, is unaffected.
  ]]
  local queue = {}
  local setters = model and model.setters or {}
  -- One function for both callers: the anker.set op (WebSocket clients) and
  -- other scripts, through profiles.anker.control.
  local function request_set(p)
    local s = setters[p.field]
    if not s then return { error = 'unknown_field', message = tostring(p.field) .. ' has no captured setter' } end
    local v, why = setter.check(s, p.value)
    if not v then return { error = 'bad_value', message = ('%s %s'):format(p.field, why) } end
    queue[#queue + 1] = { field = p.field, opcode = s.opcode, value = v, s = s }
    return { queued = true, field = p.field, value = v }
  end
  ops.register('anker.set', dispatch_set)
  control.devices[addr] = {
    who = who,
    set = function(field, value) return request_set{ field = field, value = value } end,
  }

  local function send_setters()
    while #queue > 0 do
      local q = table.remove(queue, 1)
      local _, err = link:secure(model.poll.ft, q.opcode,
        setter.payload(q.s, q.value, link_m.now_epoch()), q.opcode | 0x0800, 4000)
      if err then log.warn(('anker: set %s=%d: %s'):format(q.field, q.value, err))
      else log.info(('anker: set %s=%d: accepted'):format(q.field, q.value)) end
    end
  end

  if model and model.poll then
    -- Polled family (the power stations): ask, decode, wait.
    while true do
      local dec, perr = link:secure(model.poll.ft, model.poll.opcode,
        tlv.build{ { tag = 0xa1, enc = 'bytes', value = '\x21', type_byte = false },
                   { tag = 0xfe, enc = 'bytes',
                     value = '\x03' .. link_m.le32(link_m.now_epoch()),
                     type_byte = false } },
        model.poll.want, 6000)
      if dec then
        publish(dec)
        -- ~68 TLV item tables per report fragments the arena faster than the
        -- incremental GC keeps up with, so collect deliberately.
        local live = sys.collect()
        polls = polls + 1
        if polls <= 3 or polls % 10 == 0 then
          log.info(('anker: poll %d, live heap %d B'):format(polls, live))
        end
      else
        log.warn('anker: poll: ' .. tostring(perr))
        if not is_connected() then log.error('anker: link dropped'); return end
      end
      -- Sleep out the poll period, but wake for a queued setter.
      local wake = sys.ms() + (model.poll.period_ms or 5000)
      while #queue == 0 and sys.ms() < wake do sys.sleep(200) end
      send_setters()
    end
  else
    -- Pushing family (the chargers): just listen.
    while true do
      local f, dec = link:await(nil, 10000)
      if not f then
        if dec ~= 'timeout' then log.error('anker: link ended: ' .. tostring(dec)); return end
        if not is_connected() then log.error('anker: link dropped'); return end
      elseif model == nil or f.opcode == model.report
             or (model.also and model.also[f.opcode]) then
        publish(dec)
      end
    end
  end
end

-- One entry point, so `require` hands back the function itself and a launcher
-- reads as two lines rather than three. A session that ends -- the link
-- dropped, the device went away, a handshake failed -- starts over after a
-- pause, looking again for any device no other launcher holds. Only a
-- configuration error stops for good.
return function(target, who)
  while true do
    if R.run(target, who) == 'fatal' then return end
    sys.collect()
    sys.sleep(5000)
  end
end
