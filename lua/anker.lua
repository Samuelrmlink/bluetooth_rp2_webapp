--[[
Anker devices, identified from their advertisement.

  {"op":"script.start","params":{"script":"anker"}}

This is a launcher; the driver is profiles/anker/run.lua, which is shared so
that a second device costs a second two-line launcher rather than a second
copy of the code. To watch two at once, add lua/anker2.lua:

    local run = require 'profiles.anker.run'
    run(sys.secret('anker', 'target2'))

CREDENTIALS ARE NOT IN THIS FILE AND MUST NOT BE. They come from
cfg/anker.json, which is git-ignored and which the HTTP filesystem API will
not serve:

  { "user_id": "<40 chars>", "private_key": "<64 hex>",
    "tz": "CST6CDT,M3.2.0,M11.1.0", "tz_offset_sec": "-21600",
    "target": "B105" }

Only private_key is needed to reach a session; user_id authorises it and is
required before most devices will report. "target" is optional and pins which
device this launcher takes when several are in range -- an address or a
productType.
]]

local run = require 'profiles.anker.run'
run(nil)
