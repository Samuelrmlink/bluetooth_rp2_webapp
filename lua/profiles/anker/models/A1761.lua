--[[
SOLIX C1000(X) -- productType B105, ZX/PPS power-station family.

WHERE THESE NAMES COME FROM, and why the previous set was wrong.

The earlier version of this table was guessed from value plausibility, and it
was wrong in the way that is hardest to notice: b3 read 166, was called
"temperature_c" with a 0.1 scale, and rendered as a believable 16.6 C. It is
the main firmware version, 1.6.6. b5 and b6 both read exactly 115 and were
called battery_cycles and battery_health_pct -- two unrelated quantities do not
land on the same number; they are the inverter and MPPT firmware versions. bd
read 29 as "current_hour", which no clock can produce. ab was "battery_pct" and
sat at 0 forever, because it is a USB port's output power.

The names below come from the vendor Android app instead, where three
independent derivations agree:

  1. Tags ARE field indices. BleIndex.indexString(n) (ble_zx/constant/
     ble_index.dart:10) renders the byte n as two hex digits, and every PPS
     parser looks its fields up by that string. So tag "d1" is index 0xD1.
  2. A1771DeviceCommands.parseDeviceAllInfo (ble/device/A1771/
     a1771_device_commands.dart:10) is the 0x0840 parser, and maps each tag to
     a device-object field offset. A1753 -- the C1000's controller -- calls it
     as super and adds three tags of its own (a1753_device_commands.dart:175).
  3. A1770AnkerDevice.toString() (ble/device/A1770/a1770_anker_device.dart:
     1427) prints 62 of those same offsets WITH their Dart field names.
     A1770/A1771/A1753/A1761 all derive from one 1152-byte base, so the
     offsets denote the same fields.

A fourth check: the setter opcodes run 0x44..0x52 in exactly the order the tags
d1..de run, which derives the same sequence a different way.

Confirmed against this unit by its owner: d1 = 600 is the AC charge limit in
watts, d2 = 720 is the device timeout in minutes (12 h), and da = 60 is the AC
output frequency in Hz. bd = 29 C, c1 = 100 % SOC and c3 = 99 % SoH are all
self-consistent with a healthy idle unit.

`verified` stays FALSE. The names are read off a decompiled app, not confirmed
field by field against hardware; only the values named above are corroborated
by the device itself. Do not promote the flag without changing a setting on the
device and watching the matching tag move.

THE PORT LAYOUT IS FAMILY-SPECIFIC, and this bit off a first attempt. The
A1781 family parses the SAME tag range into a DIFFERENT set of fields -- its a9
is pd3OutputPower and its ad is car2OutputPower. The C1000 is A1753 family, so
the layout below follows A1771DeviceCommands.parseWorkInfo
(a1771_device_commands.dart:2017), whose field offsets run 0x1e7, 0x1eb, then
skip to 0x1f3: two USB-C ports, then four USB-A slots, then one car port. Do
NOT copy this table to another product code without re-deriving it from that
product's own *_device_commands.dart.

Known gaps, deliberately not papered over:
  * a1 is genuinely unknown. No handler in the A1753/A1771/A1781 path reads
    tag "a1" for this opcode, so it is in `ignore` rather than named.
  * d9 (lcdLight, "Screen Brightness") reads 128 here, but the app understands
    only 1=Low, 2=Mid, 3=High and silently shows "Mid" for anything else.
    0x80 looks like a raw backlight PWM value or a flag bit. Changing the
    brightness in the app and re-reading d9 would settle it.
  * d5/d6 are byte arrays, not scalars -- charge limit and discharge limit
    packed together -- so they are kept raw rather than decoded to a number.
  * ab/ac (usb3/usb4) and ca/cb are named from the A1771 sibling, which really
    does render four USB-A tiles. A C1000 has two, so they read 0 forever and
    cannot be confirmed on this hardware.
]]
return {
  product_code = 'A1761',
  name         = 'SOLIX C1000(X) Portable Power Station',
  family       = 'pps',
  verified     = false,      -- names from the decompiled app; see the header

  -- How to ask for a report, and what the answer looks like.
  poll = { ft = 0x0F, opcode = 0x0040, want = 0x0840, period_ms = 5000,
           payload_tlv = { { tag = 0xa1, value = '\x21' } }, fe_epoch = 'typed' },
  report = 0x0840,

  -- a1 is the selector echo, not data. fe is the sequence guard the app uses
  -- to drop stale packets (a1771_device_commands.dart:106), not a measurement.
  ignore = { a1 = true, fe = true },

  fields = {
    -- Countdown timers and BMS uptime ---------------------------------------
    a2 = { 'ac_countdown_s',        'u32' },
    a3 = { 'car_countdown_s',       'u32' },
    a4 = { 'remaining_time_h',      'u32', 0.1 },  -- bmsLastTime; was "wifi_rssi"

    -- Power flow. A1753-family port grouping: two USB-C (PD), four USB-A
    -- slots (a C1000 populates two), one car/DC port, then the totals.
    a5 = { 'ac_input_power_w',      'u16' },
    a6 = { 'ac_output_power_w',     'u16' },
    a7 = { 'pd1_output_power_w',    'u16' },
    a8 = { 'pd2_output_power_w',    'u16' },
    a9 = { 'usb1_output_power_w',   'u16' },
    aa = { 'usb2_output_power_w',   'u16' },
    ab = { 'usb3_output_power_w',   'u16' },   -- was wrongly "battery_pct"
    ac = { 'usb4_output_power_w',   'u16' },
    ad = { 'car1_output_power_w',   'u16' },   -- was wrongly "battery_voltage"
    ae = { 'solar_input_power_w',   'u16' },
    af = { 'total_input_power_w',   'u16' },
    b0 = { 'total_output_power_w',  'u16' },

    -- Identity and firmware. These are VERSIONS, not measurements: the old
    -- table read b3 as a temperature and b5/b6 as battery statistics.
    b1 = { 'device_name_hex',       'u16' },
    b2 = { 'device_capacity',       'u16' },
    b3 = { 'main_version',          'u16' },
    b4 = { 'mcu_version',           'u16' },
    b5 = { 'inverter_version',      'u16' },
    b6 = { 'mppt_version',          'u16' },
    b7 = { 'pd_version',            'u16' },
    b8 = { 'bms_version_major',     'u16' },
    b9 = { 'bms_version_minor',     'u16' },
    ba = { 'version',               'u16' },

    -- Working state ---------------------------------------------------------
    bb = { 'ac_output_state',       'u16' },
    bc = { 'ac_dc_work_state',      'u16' },   -- 0 idle 1 discharge 2 charge
                                               -- 3 sleep 4 shutdown
    bd = { 'bms_temp_major_c',      'u8'  },   -- was wrongly "current_hour"
    be = { 'bms_temp_minor_c',      'u8'  },
    bf = { 'bms_state_major',       'u8'  },
    c0 = { 'bms_state_minor',       'u8'  },
    c1 = { 'battery_pct',           'u8'  },   -- bmsEleMajor: the real SOC
    c2 = { 'battery_pct_minor',     'u8'  },
    c3 = { 'battery_health_pct',    'u8'  },   -- bmsHealthMajor: the real SoH
    c4 = { 'battery_health_minor',  'u8'  },
    c5 = { 'bms_connect_state',     'u8'  },
    c6 = { 'pd1_work_state',        'u8'  },
    c7 = { 'pd2_work_state',        'u8'  },
    c8 = { 'usb1_work_state',       'u8'  },
    c9 = { 'usb2_work_state',       'u8'  },
    ca = { 'usb3_work_state',       'u8'  },
    cb = { 'usb4_work_state',       'u8'  },
    cc = { 'car1_work_state',       'u8'  },
    cd = { 'ac_insert_switch',      'u8'  },
    ce = { 'device_error_code',     'u8'  },   -- also the temp-warning state
    cf = { 'overload_state',        'u8'  },

    -- Settings. This run IS the settings struct, and the setter opcodes
    -- 0x44..0x52 follow it in the same order.
    d0 = { 'serial_number',         'str' },
    d1 = { 'ac_charge_power_w',     'u16' },   -- CONFIRMED by the owner: 600 W
    d2 = { 'device_timeout_min',    'u16' },   -- CONFIRMED by the owner: 720 = 12 h
    d3 = { 'screen_timeout_s',      'u16' },   -- 1800 reads "30 mins" in the app
    d4 = { 'inverter_wait_time',    'u16' },   -- name certain, unit inferred
    d5 = { 'work_limits_major',     'str' },   -- [charge %, discharge %]
    d6 = { 'work_limits_minor',     'str' },   -- same, for the expansion pack
    d7 = { 'ac_output_enabled',     'u8'  },
    d8 = { 'car_output_enabled',    'u8'  },
    d9 = { 'lcd_brightness',        'u8'  },   -- app scale is 1/2/3; see header
    da = { 'ac_frequency_hz',       'u8'  },   -- CONFIRMED by the owner: 60 Hz
    db = { 'power_saving_mode',     'u8'  },
    dc = { 'ambient_light',         'u8'  },   -- 0 off,1 low,2 med,3 high,4 SOS
    dd = { 'temp_unit',             'u8'  },   -- 0 = Celsius, 1 = Fahrenheit
    de = { 'screen_enabled',        'u8'  },
    e3 = { 'is_ota',                'u8'  },
    e5 = { 'ultra_fast_charging',   'u8'  },

    -- Trailer ---------------------------------------------------------------
    f7 = { 'memory_port_bits',      'u32' },
    f8 = { 'capability_block',      'str' },   -- [0] car saving mode, [1] AC
                                               -- smart mode, [5] memory port
    f9 = { 'voltage_type',          'u8'  },
    fd = { 'sub_package',           'str' },   -- OTA target, e.g. "A1761_30Ah"
  },
}
