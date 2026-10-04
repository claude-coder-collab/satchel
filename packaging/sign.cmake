# CPack pre-build script: signs the staged binaries when signing credentials are present in the
# environment, and does nothing otherwise.
#   macOS:   SATCHEL_MACOS_SIGN_IDENTITY (a Developer ID Application identity in the keychain)
#   Windows: SATCHEL_WINDOWS_CERT (path to a .pfx) and SATCHEL_WINDOWS_CERT_PASSWORD

set(staging "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")

if(APPLE AND DEFINED ENV{SATCHEL_MACOS_SIGN_IDENTITY})
    file(GLOB_RECURSE apps LIST_DIRECTORIES true "${staging}/*.app")
    list(FILTER apps INCLUDE REGEX "\\.app$")
    foreach(app IN LISTS apps)
        file(GLOB helpers "${app}/Contents/Helpers/*")
        foreach(helper IN LISTS helpers)
            execute_process(COMMAND codesign --force --timestamp --options runtime --sign "$ENV{SATCHEL_MACOS_SIGN_IDENTITY}" "${helper}" COMMAND_ERROR_IS_FATAL ANY)
        endforeach()
        execute_process(COMMAND codesign --force --deep --timestamp --options runtime --sign "$ENV{SATCHEL_MACOS_SIGN_IDENTITY}" "${app}" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND codesign --verify --strict --deep "${app}" COMMAND_ERROR_IS_FATAL ANY)
        message(STATUS "Signed ${app}")
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
