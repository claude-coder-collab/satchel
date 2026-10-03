file(READ ${PROJECT_SOURCE_DIR}/core/include/zp/zp.h zp_header)
string(REGEX MATCHALL "ZP_API [^(]* (zp_[a-z0-9_]+)\\(" zp_decls "${zp_header}")
set(zp_exports "")
foreach(decl IN LISTS zp_decls)
    string(REGEX REPLACE ".* (zp_[a-z0-9_]+)\\($" "_\\1" name "${decl}")
    list(APPEND zp_exports "${name}")
endforeach()
list(APPEND zp_exports _malloc _free)
list(JOIN zp_exports "," zp_exports_csv)

add_executable(satchel_wasm wasm_entry.c)
set_target_properties(satchel_wasm PROPERTIES OUTPUT_NAME satchel SUFFIX ".mjs")
target_link_libraries(satchel_wasm PRIVATE zp::core)
target_link_options(satchel_wasm PRIVATE
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createSatchel
    -sENVIRONMENT=web,worker,node
    -sEXPORTED_FUNCTIONS=${zp_exports_csv}
    "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,addFunction,removeFunction,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8,getValue,setValue"
    -sALLOW_TABLE_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sMAXIMUM_MEMORY=${ZP_WASM_MAXIMUM_MEMORY}
    "-sPTHREAD_POOL_SIZE=(typeof navigator!=='undefined'&&navigator.hardwareConcurrency?Math.min(navigator.hardwareConcurrency,16):4)+3"
    -sPTHREAD_POOL_SIZE_STRICT=0
    -sNO_EXIT_RUNTIME=1
)

if(ZP_BUILD_TESTS)
    add_test(NAME js_smoke COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/test/smoke.mjs $<TARGET_FILE:satchel_wasm>)
endif()
