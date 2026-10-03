include(FetchContent)

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

set(ZLIB_COMPAT OFF CACHE BOOL "" FORCE)
set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
set(WITH_GZFILEOP OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(WITH_FUZZERS OFF CACHE BOOL "" FORCE)
set(WITH_RUNTIME_CPU_DETECTION ON CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ZLIB-NG
    URL https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz
    URL_HASH SHA256=f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1
    SYSTEM
    EXCLUDE_FROM_ALL
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(ZLIB-NG)

foreach(opt MZ_COMPAT MZ_BZIP2 MZ_LZMA MZ_PPMD MZ_ZSTD MZ_LIBCOMP MZ_FETCH_LIBS MZ_FORCE_FETCH_LIBS
            MZ_PKCRYPT MZ_WZAES MZ_OPENSSL MZ_LIBBSD MZ_ICONV MZ_BUILD_TESTS MZ_BUILD_UNIT_TESTS MZ_BUILD_FUZZ_TESTS)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(MZ_ZLIB ON CACHE BOOL "" FORCE)
set(MZ_ZLIB_FLAVOR "zlib-ng" CACHE STRING "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(minizip-ng
    URL https://github.com/zlib-ng/minizip-ng/archive/refs/tags/4.2.2.tar.gz
    URL_HASH SHA256=71af7b9799856d8b03619df3949e9c1be9703f8de0795af71399ba283cb27aac
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(minizip-ng)

set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(utf8proc
    URL https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.12.0.tar.gz
    URL_HASH SHA256=f564011d38b2888d583d510b08e69ffa15aa117155db1b9b49ef1dfe1fa25111
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(utf8proc)

foreach(opt BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_DOCS WITH_OGG WITH_ASM WITH_AVX WITH_FORTIFY_SOURCE WITH_STACK_PROTECTOR
            INSTALL_MANPAGES INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE ENABLE_MULTITHREADING)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(flac
    URL https://github.com/xiph/flac/archive/refs/tags/1.5.0.tar.gz
    URL_HASH SHA256=aea54ed186ad07a34750399cb27fc216a2b62d0ffcd6dc2e3064a3518c3146f8
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(flac)
# Identical encoder output on every platform: no SIMD paths and no FMA contraction.
if(MSVC)
    target_compile_options(FLAC PRIVATE /fp:precise)
else()
    target_compile_options(FLAC PRIVATE -ffp-contract=off)
endif()

find_package(Threads REQUIRED)
