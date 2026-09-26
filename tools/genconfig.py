#!/usr/bin/env python3
"""Turn Kconfig + .config into autoconf.h and config.cmake.

Run at CMake configure time. A fresh clone has no .config, so one is seeded
from the board defconfig and then brought up to date -- `cmake -S . -B build`
must just work without a separate configuration step.

.config is NOT committed; configs/*_defconfig are. Same split as ESP-IDF's
sdkconfig / sdkconfig.defaults.
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "third_party"))
from kconfiglib import Kconfig, BOOL, STRING, INT, HEX, TRISTATE

srctree, defconfig, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
os.environ["srctree"] = srctree
os.chdir(srctree)

kconf = Kconfig("Kconfig", warn_to_stderr=True)
dotconfig = os.path.join(outdir, ".config")

if os.path.exists(dotconfig):
    kconf.load_config(dotconfig)
elif os.path.exists(defconfig):
    kconf.load_config(defconfig)
else:
    kconf.load_allnoconfig()          # every bool n, then defaults apply
kconf.write_config(dotconfig)

os.makedirs(outdir, exist_ok=True)
kconf.write_autoconf(os.path.join(outdir, "autoconf.h"))

# CMake needs the same values to gate target_sources() and link libraries.
with open(os.path.join(outdir, "config.cmake"), "w") as f:
    f.write("# Generated from Kconfig by tools/genconfig.py -- do not edit.\n")
    for sym in kconf.unique_defined_syms:
        val = sym.str_value
        if sym.orig_type in (BOOL, TRISTATE):
            f.write('set(CONFIG_%s %s)\n' % (sym.name, "ON" if val == "y" else "OFF"))
        elif sym.orig_type in (INT, HEX):
            f.write('set(CONFIG_%s %s)\n' % (sym.name, val or 0))
        elif sym.orig_type is STRING:
            f.write('set(CONFIG_%s "%s")\n' % (sym.name, val))

print("kconfig: %s -> autoconf.h + config.cmake" % dotconfig)
