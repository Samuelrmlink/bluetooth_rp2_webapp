--[[
SOLIX C1000(X) -- productType B105, ZX/PPS power-station family.

Field map from the 0x0840 DeviceAllBaseInfo table in anker-protocol.md,
cross-checked against a live capture from this unit (firmware 0.0.0.3, serial
APC5YX0D43200118). The values that agree with the reference capture --
min_soc 60, max_discharge 99, battery_health 115, hw_version 1, idle_timeout
60, max_ac_input 720 -- are asserted individually by the frozen vector in
test/host/proto_test.lua, so this table cannot drift silently.

The body is a serialized struct tagged 0xA0 + structIndex, so tag numbering
drifts between firmware revisions. The identity tags (d0 serial, fd model,
a1 selector) are the stable ones.
]]
return {
  product_code = 'A1761',
  name         = 'SOLIX C1000(X) Portable Power Station',
  family       = 'pps',
  verified     = true,       -- decoded against this device on the bench

  -- How to ask for a report, and what the answer looks like.
  poll = { ft = 0x0F, opcode = 0x0040, want = 0x0840, period_ms = 5000,
           payload_tlv = { { tag = 0xa1, value = '\x21' } }, fe_epoch = 'typed' },
  report = 0x0840,

  -- a1 is the selector echo, not data.
  ignore = { a1 = true },

  fields = {
    a2 = { 'counter_a2',            'u32' },
    a3 = { 'counter_a3',            'u32' },
    a4 = { 'wifi_rssi',             's16' },
    a5 = { 'ac_power_in_w',         'u16' },
    a6 = { 'ac_power_out_w',        'u16' },
    a7 = { 'dc_power_out_w',        'u16' },
    a8 = { 'type_c_power_out_w',    'u16' },
    a9 = { 'usb_power_out_w',       'u16' },
    aa = { 'total_output_w',        'u16' },
    ab = { 'battery_pct',           'u16' },
    ac = { 'battery_power_w',       's16' },   -- + charging / - discharging
    ad = { 'battery_voltage',       'u16', 0.01 },
    ae = { 'solar_power_in_w',      'u16' },
    af = { 'ac_switch',             'u16' },
    b0 = { 'dc_switch',             'u16' },
    b1 = { 'charging_state',        'u16' },
    b2 = { 'online_status',         'u16' },
    b3 = { 'temperature_c',         'u16', 0.1 },
    b4 = { 'error_code',            'u16' },
    b5 = { 'battery_cycles',        'u16' },
    b6 = { 'battery_health_pct',    'u16' },
    b8 = { 'battery_resistance',    'u16' },
    ba = { 'battery_temp_c',        'u16', 0.1 },
    bb = { 'ac_output_active',      'u8'  },
    bc = { 'led_mode',              'u8'  },
    bd = { 'current_hour',          'u8'  },
    c0 = { 'charge_speed',          'u8'  },
    c1 = { 'max_charge_soc_pct',    'u8'  },
    c3 = { 'max_discharge_soc_pct', 'u8'  },
    c7 = { 'solar_pv1_w',           'u8'  },
    c8 = { 'solar_pv2_w',           'u8'  },
    cc = { 'dc_output_active',      'u8'  },
    d0 = { 'serial_number',         'str' },
    d1 = { 'capacity_wh',           'u16' },
    d2 = { 'max_ac_input_w',        'u16' },
    d3 = { 'display_timeout_s',     'u16' },
    d4 = { 'idle_timeout_min',      'u16' },
    d5 = { 'ups_mode',              'u16' },
    d6 = { 'ups_reserve_pct',       'u16' },
    d7 = { 'ac_enabled',            'u8'  },
    d8 = { 'dc_enabled',            'u8'  },
    d9 = { 'light_mode',            'u8'  },   -- 0 off, 1 on, 2 auto
    da = { 'min_soc_pct',           'u8'  },
    f9 = { 'hw_version',            'u8'  },
    fd = { 'model_name',            'str' },
  },
}
