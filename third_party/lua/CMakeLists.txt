# Lua 5.4.8, trimmed for an embedded target.
#
# Dropped, deliberately rather than stubbed, so a script cannot reach for them:
#   liolib.c   files -- the filesystem is reached through our own bindings
#   loslib.c   os.* -- brings in time/locale/process; os.time and os.clock are
#              provided by src/lua/lbind_sys.c instead
#   loadlib.c  package/require/dlopen -- require() is ours, because module
#              resolution is the draft -> staged -> committed overlay
#   ldblib.c   the debug library, which can defeat the sandbox
#   lua.c/luac.c  standalone interpreter and compiler
add_library(lua STATIC
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldebug.c ldo.c
    ldump.c lfunc.c lgc.c llex.c lmathlib.c lmem.c lobject.c lopcodes.c
    lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c
    lundump.c lutf8lib.c lvm.c lzio.c
)
target_include_directories(lua PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(lua PRIVATE -include ${CMAKE_CURRENT_LIST_DIR}/lua_pico_conf.h -Wno-unused-function)
target_compile_definitions(lua PUBLIC LUA_32BITS=1)
target_link_libraries(lua PUBLIC pico_stdlib)
