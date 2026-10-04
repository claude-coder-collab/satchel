add_library(satchel_gui_logic STATIC logic/logic.cpp)
target_include_directories(satchel_gui_logic PUBLIC logic)
target_link_libraries(satchel_gui_logic PUBLIC zp::project_options)

if(ZP_BUILD_TESTS)
    add_executable(satchel_gui_logic_tests ${PROJECT_SOURCE_DIR}/tests/gui/test_logic.cpp)
    target_link_libraries(satchel_gui_logic_tests PRIVATE satchel_gui_logic Catch2::Catch2WithMain)
    catch_discover_tests(satchel_gui_logic_tests DISCOVERY_MODE PRE_TEST)
endif()

option(ZP_BUILD_GUI "Build the Qt desktop app (needs Qt 6 Widgets)" ON)
if(ZP_BUILD_GUI)
    find_package(Qt6 6.4 QUIET COMPONENTS Widgets Test)
    if(NOT Qt6_FOUND)
        message(STATUS "Qt 6 not found: the desktop app is not built")
    endif()
endif()
if(ZP_BUILD_GUI AND Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_library(satchel_gui_app STATIC
        app/app.cpp
        app/jobs.cpp
        app/settings_store.cpp
        app/archive_model.cpp
        app/dialogs.cpp
        app/simple_window.cpp
        app/main_window.cpp
    )
    target_include_directories(satchel_gui_app PUBLIC app ${PROJECT_SOURCE_DIR}/desktop/common)
    target_link_libraries(satchel_gui_app PUBLIC satchel_gui_logic zp::core Qt6::Widgets)

    qt_add_executable(satchel_gui app/main.cpp)
    set_target_properties(satchel_gui PROPERTIES OUTPUT_NAME "$<IF:$<PLATFORM_ID:Darwin>,Satchel,satchel-gui>" WIN32_EXECUTABLE ON)
    target_link_libraries(satchel_gui PRIVATE satchel_gui_app)
    zp_configure_bundle(satchel_gui)
    zp_add_windows_resources(satchel_gui "Satchel")

    if(ZP_BUILD_TESTS AND TARGET Qt6::Test)
        qt_add_executable(satchel_gui_tests ${PROJECT_SOURCE_DIR}/tests/gui/test_app.cpp)
        target_link_libraries(satchel_gui_tests PRIVATE satchel_gui_app Qt6::Test)
        add_test(NAME satchel_gui_tests COMMAND satchel_gui_tests)
        set_tests_properties(satchel_gui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
    install(TARGETS satchel_gui BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    zp_install_linux_desktop_files()

    if(APPLE OR WIN32)
        set(deploy_default ON)
    else()
        set(deploy_default OFF)
    endif()
    option(ZP_DEPLOY_QT "Copy the Qt libraries next to the installed app (macdeployqt/windeployqt)" ${deploy_default})
    if(ZP_DEPLOY_QT)
        qt_generate_deploy_app_script(TARGET satchel_gui OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR NO_TRANSLATIONS)
        install(SCRIPT ${deploy_script})
    endif()
endif()
