# Install layout, platform resources and CPack configuration for the desktop apps.
# Placeholder identity values are listed in docs/IMPLEMENTATION.md, section 1.

include(GNUInstallDirs)

set(ZP_VENDOR "Venn Audio Ltd.")
set(ZP_BUNDLE_ID "com.vennaudio.satchel" CACHE STRING "macOS bundle identifier (placeholder)")
set(ZP_WIX_UPGRADE_GUID "C228FD8D-4C42-4523-90CC-05246E0FF58B")
set(ZP_PACKAGE_CONTACT "Venn Audio Ltd. <packages@example.invalid>" CACHE STRING "Debian maintainer (placeholder)")
set(ZP_ICON_DIR "${PROJECT_SOURCE_DIR}/packaging/icons")
set(ZP_ICON_ICO "${ZP_ICON_DIR}/satchel.ico")
set(ZP_UPDATE_FEED_URL_MACOS "https://claude-coder-collab.github.io/satchel/appcast-macos.xml" CACHE STRING "Sparkle feed (placeholder)")
set(ZP_UPDATE_FEED_URL_WINDOWS "https://claude-coder-collab.github.io/satchel/appcast-windows.xml" CACHE STRING "WinSparkle feed (placeholder)")
set(ZP_UPDATE_PUBLIC_KEY "Q0RkfO0SbAmepgAaM3ogueEJAZ+bGAqXjl+FdIh9UFU=" CACHE STRING "EdDSA public key of update signatures (placeholder; its private key was discarded)")
option(ZP_BUNDLE_UPDATER "Download Sparkle (macOS) or WinSparkle (Windows) and install it with the app" OFF)

if(CMAKE_OSX_DEPLOYMENT_TARGET)
    set(ZP_MACOS_MINIMUM_ENTRY "<key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>")
endif()

# Adds the icon and version resource to a Windows executable.
function(zp_add_windows_resources target description)
    if(NOT WIN32)
        return()
    endif()
    set(ZP_RC_DESCRIPTION "${description}")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
    configure_file("${PROJECT_SOURCE_DIR}/packaging/windows/satchel.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
endfunction()

# Turns a Qt executable into the Satchel.app bundle on macOS.
function(zp_configure_bundle target)
    if(NOT APPLE)
        return()
    endif()
    set(icns "${ZP_ICON_DIR}/satchel.icns")
    target_sources(${target} PRIVATE "${icns}")
    set_source_files_properties("${icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${ZP_BUNDLE_ID}"
        MACOSX_BUNDLE_ICON_FILE satchel.icns
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2026 ${ZP_VENDOR}")
endfunction()

# Installs the update framework next to the app (packages only; the app runs without it).
function(zp_install_updater)
    if(NOT ZP_BUNDLE_UPDATER)
        return()
    endif()
    include(FetchContent)
    if(APPLE)
        FetchContent_Declare(sparkle
            URL https://github.com/sparkle-project/Sparkle/releases/download/2.10.0/Sparkle-2.10.0.tar.xz
            URL_HASH SHA256=c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c)
        FetchContent_MakeAvailable(sparkle)
        install(CODE "execute_process(COMMAND ditto \"${sparkle_SOURCE_DIR}/Sparkle.framework\" \"\${CMAKE_INSTALL_PREFIX}/Satchel.app/Contents/Frameworks/Sparkle.framework\" COMMAND_ERROR_IS_FATAL ANY)")
    elseif(WIN32)
        FetchContent_Declare(winsparkle
            URL https://github.com/vslavik/winsparkle/releases/download/v0.9.4/WinSparkle-0.9.4.zip
            URL_HASH SHA256=6037df37fc263bd1650a1c4949681a9d40ffe991d01f35892a406cb5d103c976)
        FetchContent_MakeAvailable(winsparkle)
        install(FILES "${winsparkle_SOURCE_DIR}/WinSparkle-0.9.4/x64/Release/WinSparkle.dll" DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endfunction()

function(zp_install_macos_uninstaller)
    if(NOT APPLE)
        return()
    endif()
    install(PROGRAMS "${PROJECT_SOURCE_DIR}/packaging/macos/Uninstall Satchel.command" "${PROJECT_SOURCE_DIR}/packaging/macos/uninstall.py" DESTINATION .)
endfunction()

function(zp_install_linux_desktop_files)
    if(APPLE OR WIN32)
        return()
    endif()
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/satchel.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${ZP_ICON_DIR}/satchel.png" DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps")
    install(FILES "${ZP_ICON_DIR}/satchel-512.png" DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps" RENAME satchel.png)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/dolphin/satchel-compress.desktop" "${PROJECT_SOURCE_DIR}/packaging/linux/dolphin/satchel-extract.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/kio/servicemenus")
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/nautilus/satchel.py" DESTINATION "${CMAKE_INSTALL_DATADIR}/nautilus-python/extensions")
endfunction()

# App-local MSVC runtime (msvcp140, vcruntime140…) next to the executables and the shell DLL.
function(zp_install_msvc_runtime)
    if(NOT MSVC)
        return()
    endif()
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
    include(InstallRequiredSystemLibraries)
endfunction()

macro(zp_configure_cpack)
    configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_BINARY_DIR}/LICENSE.txt" COPYONLY)

    set(CPACK_PACKAGE_NAME "satchel")
    set(CPACK_PACKAGE_VENDOR "${ZP_VENDOR}")
    set(CPACK_PACKAGE_VERSION "${ZP_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/claude-coder-collab/satchel")
    set(CPACK_PACKAGE_CONTACT "${ZP_PACKAGE_CONTACT}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Satchel")
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_BINARY_DIR}/LICENSE.txt")
    set(CPACK_STRIP_FILES ON)
    set(CPACK_PRE_BUILD_SCRIPTS "${PROJECT_SOURCE_DIR}/packaging/sign.cmake")
    set(CPACK_ZP_APPEX_ENTITLEMENTS "${PROJECT_SOURCE_DIR}/desktop/macos/quicklook/entitlements.plist")
    set(package_arch "${CMAKE_SYSTEM_PROCESSOR}")
    list(LENGTH CMAKE_OSX_ARCHITECTURES arch_count)
    if(APPLE AND arch_count GREATER 1)
        set(package_arch universal)
    endif()
    set(CPACK_PACKAGE_FILE_NAME "Satchel-${ZP_VERSION}-${CMAKE_SYSTEM_NAME}-${package_arch}")

    if(APPLE)
        set(CPACK_GENERATOR "DragNDrop")
        set(CPACK_DMG_VOLUME_NAME "Satchel ${ZP_VERSION}")
        set(CPACK_DMG_FORMAT "UDZO")
    elseif(WIN32)
        set(CPACK_GENERATOR "WIX")
        set(CPACK_WIX_VERSION 4)
        set(CPACK_WIX_UPGRADE_GUID "${ZP_WIX_UPGRADE_GUID}")
        set(CPACK_WIX_PRODUCT_ICON "${ZP_ICON_ICO}")
        set(CPACK_WIX_PROGRAM_MENU_FOLDER "Satchel")
        set(CPACK_WIX_ARCHITECTURE "x64")
        set(CPACK_WIX_PATCH_FILE "${PROJECT_SOURCE_DIR}/packaging/windows/wix_patch.xml")
        set(CPACK_PACKAGE_EXECUTABLES "satchel-gui;Satchel")
    else()
        set(CPACK_GENERATOR "DEB;TGZ")
        set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
        set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
        set(CPACK_DEBIAN_PACKAGE_SUGGESTS "python3-nautilus")
        set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Lossless media packaging as standard zip archives.\n Creates ordinary zip files; WAV, AIFF, CAF, RF64 and Wave64 audio is stored as\n FLAC and restored bit-exactly on extraction.")
    endif()
    include(CPack)
endmacro()
