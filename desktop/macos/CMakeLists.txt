# macOS integration: the Quick Look preview extension, embedded in Satchel.app/Contents/PlugIns.
enable_language(OBJC)

add_executable(satchel_quicklook quicklook/PreviewProvider.m)
configure_file(quicklook/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/quicklook/Info.plist @ONLY)
set_target_properties(satchel_quicklook PROPERTIES
    OUTPUT_NAME SatchelPreview
    MACOSX_BUNDLE ON
    BUNDLE_EXTENSION appex
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/quicklook/Info.plist
    XCODE_PRODUCT_TYPE com.apple.product-type.app-extension)
target_compile_options(satchel_quicklook PRIVATE -fobjc-arc -fapplication-extension)
target_link_options(satchel_quicklook PRIVATE "LINKER:-e,_NSExtensionMain" -fapplication-extension "SHELL:-framework Foundation" "SHELL:-framework QuickLookUI" "SHELL:-framework UniformTypeIdentifiers" -lobjc)
target_link_libraries(satchel_quicklook PRIVATE zp::core)
add_custom_command(TARGET satchel_quicklook POST_BUILD
    COMMAND codesign --force --sign - --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/quicklook/entitlements.plist $<TARGET_BUNDLE_DIR:satchel_quicklook>
    VERBATIM)

if(TARGET satchel_gui)
    add_custom_target(satchel_embed_quicklook ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:satchel_gui>/PlugIns
        COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:satchel_quicklook> $<TARGET_BUNDLE_CONTENT_DIR:satchel_gui>/PlugIns/SatchelPreview.appex
        DEPENDS satchel_gui satchel_quicklook
        VERBATIM)
    install(TARGETS satchel_quicklook BUNDLE DESTINATION Satchel.app/Contents/PlugIns)
endif()

if(ZP_BUILD_TESTS)
    add_executable(satchel_quicklook_tests ${PROJECT_SOURCE_DIR}/tests/macos/test_quicklook.m quicklook/PreviewProvider.m)
    target_compile_options(satchel_quicklook_tests PRIVATE -fobjc-arc)
    target_link_libraries(satchel_quicklook_tests PRIVATE zp::core "-framework Foundation" "-framework QuickLookUI" "-framework UniformTypeIdentifiers" objc)
    add_test(NAME satchel_quicklook_tests COMMAND satchel_quicklook_tests)
endif()
