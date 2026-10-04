configure_file(src/common/product.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/product.cpp @ONLY)

add_library(zp_core STATIC
    src/common/bytes.cpp
    src/common/json_writer.cpp
    src/common/main_thread.cpp
    src/common/status.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/product.cpp
    src/capi/zp.cpp
    src/codecs/codec.cpp
    src/codecs/deflate.cpp
    src/codecs/flac/flac_codec.cpp
    src/codecs/flac/flac_file.cpp
    src/codecs/flac/flac_format.cpp
    src/codecs/flac/restore.cpp
    src/codecs/pcm/pcm_container.cpp
    src/codecs/pcm/tags.cpp
    src/crypto/hash.cpp
    src/io/file_system.cpp
    src/io/input_source.cpp
    src/io/stream.cpp
    src/io/zip/archive_metadata.cpp
    src/io/zip/build_reader.cpp
    src/io/zip/builder.cpp
    src/io/zip/editor.cpp
    src/io/zip/extractor.cpp
    src/io/zip/minizip_adapter.cpp
    src/io/zip/output_sink.cpp
    src/io/zip/path_policy.cpp
    src/io/zip/planner.cpp
    src/io/zip/readme.cpp
    src/io/zip/reader.cpp
    src/io/zip/zip_writer.cpp
    src/pipeline/context.cpp
)
add_library(zp::core ALIAS zp_core)
target_include_directories(zp_core PUBLIC src include)
target_link_libraries(zp_core
    PUBLIC zp::project_options Threads::Threads
    PRIVATE MINIZIP::minizip zlib-ng::zlibstatic utf8proc FLAC::FLAC)
target_compile_definitions(zp_core PUBLIC UTF8PROC_STATIC)

if(MSVC)
    target_compile_definitions(zp_core PRIVATE ZP_SHARED_BUILD)
endif()

# Shared library for the PHP and Python bindings: the C API only.
if(NOT EMSCRIPTEN)
    add_library(zp_shared SHARED src/capi/shared_entry.c)
    set_target_properties(zp_shared PROPERTIES OUTPUT_NAME satchel C_VISIBILITY_PRESET hidden VERSION ${PROJECT_VERSION} SOVERSION 0)
    target_include_directories(zp_shared PUBLIC include)
    target_link_libraries(zp_shared PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,zp_core>")
    if(APPLE)
        target_link_options(zp_shared PRIVATE "-Wl,-exported_symbol,_zp_*")
    elseif(UNIX)
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/satchel.map "{ global: zp_*; local: *; };\n")
        target_link_options(zp_shared PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/satchel.map")
    endif()
endif()
