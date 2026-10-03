configure_file(src/common/product.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/product.cpp @ONLY)

add_library(zp_core STATIC
    src/common/bytes.cpp
    src/common/status.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/product.cpp
    src/capi/zp.cpp
    src/codecs/codec.cpp
    src/codecs/deflate.cpp
    src/io/file_system.cpp
    src/io/input_source.cpp
    src/io/stream.cpp
    src/io/zip/archive_metadata.cpp
    src/io/zip/builder.cpp
    src/io/zip/editor.cpp
    src/io/zip/extractor.cpp
    src/io/zip/minizip_adapter.cpp
    src/io/zip/output_sink.cpp
    src/io/zip/path_policy.cpp
    src/io/zip/planner.cpp
    src/io/zip/reader.cpp
    src/io/zip/zip_writer.cpp
    src/pipeline/context.cpp
)
add_library(zp::core ALIAS zp_core)
target_include_directories(zp_core PUBLIC src include)
target_link_libraries(zp_core
    PUBLIC zp::project_options Threads::Threads
    PRIVATE MINIZIP::minizip zlib-ng::zlibstatic utf8proc)
target_compile_definitions(zp_core PUBLIC UTF8PROC_STATIC)
