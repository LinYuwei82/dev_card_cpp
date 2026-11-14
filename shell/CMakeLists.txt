add_executable(DevCard MACOSX_BUNDLE mac/main.mm)
target_link_libraries(DevCard PRIVATE devcard_server "-framework Cocoa" "-framework WebKit")

set_target_properties(DevCard PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "DevCard"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.anye.devcard.new"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${DEVCARD_VERSION_STRING}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${DEVCARD_VERSION_STRING}"
)
