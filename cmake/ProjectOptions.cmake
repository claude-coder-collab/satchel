option(ZP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ZP_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ZP_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ZP_BUILD_TESTS "Build tests" ON)

add_library(zp_project_options INTERFACE)
add_library(zp::project_options ALIAS zp_project_options)
target_compile_features(zp_project_options INTERFACE cxx_std_${ZP_CXX_STANDARD})

if(MSVC)
    target_compile_options(zp_project_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    target_compile_definitions(zp_project_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(ZP_WARNINGS_AS_ERRORS)
        target_compile_options(zp_project_options INTERFACE /WX)
    endif()
else()
    target_compile_options(zp_project_options INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
        $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual>
        -Wcast-align -Wdouble-promotion
        "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wnull-dereference;-Wno-missing-designated-field-initializers>"
        "$<$<CXX_COMPILER_ID:GNU>:-Wno-missing-field-initializers>" -Wformat=2 -Wimplicit-fallthrough -Wundef)
    if(ZP_WARNINGS_AS_ERRORS)
        target_compile_options(zp_project_options INTERFACE -Werror)
    endif()
endif()

set(ZP_SANITIZER_FLAGS "")
if(ZP_ENABLE_SANITIZERS)
    set(ZP_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
elseif(ZP_ENABLE_TSAN)
    set(ZP_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()
if(ZP_SANITIZER_FLAGS)
    add_compile_options(${ZP_SANITIZER_FLAGS})
    add_link_options(${ZP_SANITIZER_FLAGS})
endif()

if(EMSCRIPTEN)
    add_compile_options(-pthread)
    add_link_options(-pthread -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB)
endif()
