file(READ ${PROJECT_SOURCE_DIR}/core/include/zp/zp.h zp_header)
string(REGEX MATCHALL "ZP_API [^(]* (zp_[a-z0-9_]+)\\(" zp_decls "${zp_header}")
set(zp_exports "")
foreach(decl IN LISTS zp_decls)
    string(REGEX REPLACE ".* (zp_[a-z0-9_]+)\\($" "_\\1" name "${decl}")
    list(APPEND zp_exports "${name}")
endforeach()
list(APPEND zp_exports _malloc _free _zp_wasm_mount_opfs)
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
    "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,addFunction,removeFunction,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8,getValue,setValue,FS"
    -sALLOW_TABLE_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sMAXIMUM_MEMORY=${ZP_WASM_MAXIMUM_MEMORY}
    "-sPTHREAD_POOL_SIZE=(typeof navigator!=='undefined'&&navigator.hardwareConcurrency?Math.min(navigator.hardwareConcurrency,16):4)+3"
    -sPTHREAD_POOL_SIZE_STRICT=0
    -sNO_EXIT_RUNTIME=1
    -sWASMFS=1
    -sFORCE_FILESYSTEM=1
)

if(ZP_BUILD_TESTS)
    add_test(NAME js_smoke COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/test/smoke.mjs $<TARGET_FILE:satchel_wasm>)
    add_test(NAME js_glue COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/test/glue.mjs $<TARGET_FILE:satchel_wasm>)
endif()

# The browser app: static files plus the module and the JavaScript binding.
set(ZP_WEB_DIR ${CMAKE_BINARY_DIR}/web)
set(ZP_WEB_SOURCES index.html app.mjs worker.mjs style.css coi-serviceworker.js)
add_custom_target(satchel_web ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZP_WEB_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:satchel_wasm> $<TARGET_FILE_DIR:satchel_wasm>/satchel.wasm ${ZP_WEB_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/src/satchel.mjs ${ZP_WEB_DIR}/satchel-api.mjs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ZP_WEB_SOURCES} ${ZP_WEB_DIR}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/apps/web
    DEPENDS satchel_wasm
    VERBATIM)

# npm package: the binding as index.mjs next to the Emscripten module.
set(ZP_NPM_DIR ${CMAKE_BINARY_DIR}/npm)
configure_file(package.json.in ${ZP_NPM_DIR}/package.json @ONLY)
add_custom_target(satchel_npm
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:satchel_wasm> $<TARGET_FILE_DIR:satchel_wasm>/satchel.wasm ${ZP_NPM_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/src/satchel.mjs ${ZP_NPM_DIR}/index.mjs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/README.md ${ZP_NPM_DIR}/README.md
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/LICENSE ${ZP_NPM_DIR}/LICENSE
    DEPENDS satchel_wasm
    VERBATIM)
