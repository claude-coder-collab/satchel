# macOS integration: the Quick Look preview extension, embedded in Satchel.app/Contents/PlugIns.
enable_language(Swift)

set(ZP_SATCHELC_DIR ${CMAKE_CURRENT_BINARY_DIR}/satchelc)
file(WRITE ${ZP_SATCHELC_DIR}/module.modulemap "module SatchelC {\n    header \"${PROJECT_SOURCE_DIR}/core/include/zp/zp.h\"\n    export *\n}\n")

add_executable(satchel_quicklook quicklook/PreviewProvider.swift)
configure_file(quicklook/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/quicklook/Info.plist @ONLY)
set_target_properties(satchel_quicklook PROPERTIES
    OUTPUT_NAME SatchelPreview
    MACOSX_BUNDLE ON
    BUNDLE_EXTENSION appex
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/quicklook/Info.plist
    Swift_MODULE_NAME SatchelPreview
    XCODE_PRODUCT_TYPE com.apple.product-type.app-extension)
target_compile_options(satchel_quicklook PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-I${ZP_SATCHELC_DIR};-swift-version;5;-application-extension;-parse-as-library>")
target_link_options(satchel_quicklook PRIVATE "SHELL:-Xlinker -e -Xlinker _NSExtensionMain" -application-extension "SHELL:-framework QuickLookUI" "SHELL:-framework UniformTypeIdentifiers")
target_link_libraries(satchel_quicklook PRIVATE zp::core)
add_custom_command(TARGET satchel_quicklook POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE:satchel_quicklook>.d
    COMMAND codesign --force --sign - --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/quicklook/entitlements.plist $<TARGET_BUNDLE_DIR:satchel_quicklook>
    VERBATIM)

add_custom_target(satchel_embed_quicklook ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:satchel_gui>/PlugIns
    COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:satchel_quicklook> $<TARGET_BUNDLE_CONTENT_DIR:satchel_gui>/PlugIns/SatchelPreview.appex
    DEPENDS satchel_gui satchel_quicklook
    VERBATIM)
install(TARGETS satchel_quicklook BUNDLE DESTINATION Satchel.app/Contents/PlugIns)
