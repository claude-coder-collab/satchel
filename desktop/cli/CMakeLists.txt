include(FetchContent)
FetchContent_Declare(CLI11
    URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
    URL_HASH SHA256=46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(CLI11 nlohmann_json)

add_executable(satchel_cli main.cpp common.cpp create.cpp archive.cpp)
set_target_properties(satchel_cli PROPERTIES OUTPUT_NAME satchel)
target_include_directories(satchel_cli PRIVATE ${PROJECT_SOURCE_DIR}/desktop/common)
target_link_libraries(satchel_cli PRIVATE zp::core zp::project_options CLI11::CLI11 nlohmann_json::nlohmann_json)
zp_add_windows_resources(satchel_cli "Satchel command-line tool")

if(APPLE)
    add_library(satchel_macos_uninstall STATIC macos/uninstall.cpp)
    target_include_directories(satchel_macos_uninstall PUBLIC macos)
    target_compile_definitions(satchel_macos_uninstall PUBLIC SATCHEL_BUNDLE_ID="${ZP_BUNDLE_ID}")
    target_link_libraries(satchel_macos_uninstall PRIVATE zp::project_options)
    target_sources(satchel_cli PRIVATE macos/command.cpp)
    target_link_libraries(satchel_cli PRIVATE satchel_macos_uninstall)
endif()
