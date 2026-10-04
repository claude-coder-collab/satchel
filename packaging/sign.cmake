# CPack pre-build script: signs the staged binaries. macOS bundles are always signed inside-out
# (ad-hoc without an identity) so the Quick Look extension keeps its sandbox entitlements; Windows
# binaries only when a certificate is given.
#   macOS:   SATCHEL_MACOS_SIGN_IDENTITY (a Developer ID Application identity in the keychain)
#   Windows: SATCHEL_WINDOWS_CERT (path to a .pfx) and SATCHEL_WINDOWS_CERT_PASSWORD

set(staging "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")

if(APPLE)
    if(DEFINED ENV{SATCHEL_MACOS_SIGN_IDENTITY})
        set(identity "$ENV{SATCHEL_MACOS_SIGN_IDENTITY}")
        set(flags --force --timestamp --options runtime)
    else()
        set(identity "-")
        set(flags --force)
    endif()
    file(GLOB_RECURSE apps LIST_DIRECTORIES true "${staging}/*.app")
    list(FILTER apps INCLUDE REGEX "\\.app$")
    list(FILTER apps EXCLUDE REGEX "\\.app/.*\\.app$")
    foreach(app IN LISTS apps)
        file(GLOB_RECURSE nested "${app}/Contents/Frameworks/*.dylib" "${app}/Contents/PlugIns/*.dylib" "${app}/Contents/Helpers/*")
        file(GLOB frameworks LIST_DIRECTORIES true "${app}/Contents/Frameworks/*.framework")
        file(GLOB framework_helpers LIST_DIRECTORIES true "${app}/Contents/Frameworks/*.framework/Versions/*/XPCServices/*.xpc"
             "${app}/Contents/Frameworks/*.framework/Versions/*/Autoupdate" "${app}/Contents/Frameworks/*.framework/Versions/*/Updater.app")
        foreach(helper IN LISTS framework_helpers)
            execute_process(COMMAND codesign ${flags} --preserve-metadata=entitlements --sign "${identity}" "${helper}" COMMAND_ERROR_IS_FATAL ANY)
        endforeach()
        foreach(item IN LISTS nested frameworks)
            execute_process(COMMAND codesign ${flags} --sign "${identity}" "${item}" COMMAND_ERROR_IS_FATAL ANY)
        endforeach()
        file(GLOB extensions LIST_DIRECTORIES true "${app}/Contents/PlugIns/*.appex")
        foreach(appex IN LISTS extensions)
            execute_process(COMMAND codesign ${flags} --entitlements "${CPACK_ZP_APPEX_ENTITLEMENTS}" --sign "${identity}" "${appex}" COMMAND_ERROR_IS_FATAL ANY)
        endforeach()
        execute_process(COMMAND codesign ${flags} --sign "${identity}" "${app}" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND codesign --verify --strict --deep "${app}" COMMAND_ERROR_IS_FATAL ANY)
        message(STATUS "Signed ${app} (${identity})")
    endforeach()
endif()

if(WIN32 AND DEFINED ENV{SATCHEL_WINDOWS_CERT})
    file(GLOB_RECURSE binaries "${staging}/*.exe" "${staging}/*.dll")
    if(binaries)
        execute_process(COMMAND signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256
                                /f "$ENV{SATCHEL_WINDOWS_CERT}" /p "$ENV{SATCHEL_WINDOWS_CERT_PASSWORD}" ${binaries}
                        COMMAND_ERROR_IS_FATAL ANY)
        message(STATUS "Signed ${binaries}")
    endif()
endif()
