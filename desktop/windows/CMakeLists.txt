# Windows shell integration: preview handler and Explorer commands (in-process COM server).
add_library(satchel_shell_impl STATIC shell_extension.cpp)
target_include_directories(satchel_shell_impl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(satchel_shell_impl PUBLIC UNICODE _UNICODE)
target_link_libraries(satchel_shell_impl PUBLIC zp::core zp::project_options comctl32 shlwapi shell32 ole32 propsys uuid)

add_library(satchel_shell SHARED dll_main.cpp satchel_shell.def)
target_link_libraries(satchel_shell PRIVATE satchel_shell_impl)
zp_add_windows_resources(satchel_shell "Satchel shell extension")
install(TARGETS satchel_shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(ZP_BUILD_TESTS)
    add_executable(satchel_shell_tests
        ${PROJECT_SOURCE_DIR}/tests/windows/test_shell_extension.cpp
        ${PROJECT_SOURCE_DIR}/tests/support/test_support.cpp
        ${PROJECT_SOURCE_DIR}/tests/support/pcm_fixtures.cpp)
    target_include_directories(satchel_shell_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(satchel_shell_tests PRIVATE satchel_shell_impl Catch2::Catch2WithMain)
    catch_discover_tests(satchel_shell_tests DISCOVERY_MODE PRE_TEST)
endif()
