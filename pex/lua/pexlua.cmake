# The optional Lua layer (pexlua): only for firmware that has Lua.
#   include(<repo>/pex/lua/pexlua.cmake)
#   pexlua_add(myfw)          # after pex_runtime_add(myfw); the target must see lua.h
include(${CMAKE_CURRENT_LIST_DIR}/../runtime/pex_runtime.cmake)
function(pexlua_add target)
    pex_group_table(lua _t)
    set(_s ${PEX_ROOT}/lua/pexlua.c ${_t})
    target_sources(${target} PRIVATE ${_s})
    set_property(SOURCE ${_s} APPEND PROPERTY COMPILE_OPTIONS -Os)
    target_include_directories(${target} PRIVATE ${PEX_ROOT}/lua)
endfunction()
