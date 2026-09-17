set(TARGET_NAME whatsmeow)
add_library(${TARGET_NAME} SHARED 
    "gowhatsapp.h"
    "init.c"
    "login.c"
    "qrcode.c"
    "bridge.c"
    "../bridge.c" # this needs to be compiled again since go's c-archive mode does not include the C object
    "../bridge.h"
    "process_message.c"
    "display_message.c"
    "groups.c"
    "blist.c"
    "send_message.c"
    "handle_attachment.c"
    "quarantine.m"
    "send_file.c"
    "presence.c"
    "options.c"
    "receipt.c"
    "reaction.c"
    "history.c"
    "pixbuf.c"
    "pixbuf.h"
    "commands.c"
)
add_dependencies(${TARGET_NAME} libwhatsmeow)

target_compile_definitions(${TARGET_NAME} PRIVATE PLUGIN_VERSION=${VERSION})
target_include_directories(${TARGET_NAME} PRIVATE ${GO_WHATSAPP_INCLUDE_DIRS} ${PURPLE_INCLUDE_DIRS} ${PIXBUF_INCLUDE_DIRS} ${OPUSFILE_INCLUDE_DIRS})
target_link_directories(${TARGET_NAME} PRIVATE ${PURPLE_LIBRARY_DIRS} ${PIXBUF_LIBRARY_DIRS})
target_link_libraries(${TARGET_NAME} PRIVATE ${GO_WHATSAPP_LIBRARIES} ${PURPLE_LIBRARIES} ${PIXBUF_LIBRARIES} ${OPUSFILE_LINK_LIBRARIES})
set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "lib")
if (WIN32 AND CMAKE_C_COMPILER_ID STREQUAL GNU AND CMAKE_C_COMPILER_VERSION VERSION_GREATER 4.7.2) # might actually work without this up to 7.1.0, but I did not check
    target_link_options(${TARGET_NAME} PRIVATE "-static-libgcc")
endif()
if (APPLE)
    target_link_libraries(${TARGET_NAME} PRIVATE "-framework CoreFoundation" "-framework Security" "-framework Foundation" "-framework CoreServices" resolv)
    set_source_files_properties("quarantine.m" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # Adium renders images itself, so every image type goes inline and the
    # gdk-pixbuf loader is never asked whether it could show one.
    target_compile_definitions(${TARGET_NAME} PRIVATE GOWHATSAPP_INLINE_ALL_IMAGES)

    # Adium loads its OWN libpurple and glib from the app bundle. Left at the
    # build-tree paths the linker records, this plug-in would drag in a SECOND
    # copy of each and register its protocol there, where the application never
    # looks: it probes and loads without a single error and WhatsApp simply
    # never connects. Rewriting the references to the bundle's frameworks makes
    # it share one instance, like every other bundled protocol plug-in. The
    # ad-hoc codesign afterwards is what keeps Xcode's copy phase from
    # stripping the Go runtime out of the binary on its way into the bundle.
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND install_name_tool
            -change "${PURPLE_LIBRARY_DIRS}/libpurple.0.dylib" "@executable_path/../Frameworks/libpurple.framework/Versions/0/libpurple"
            -change "${PURPLE_LIBRARY_DIRS}/libglib-2.0.0.dylib" "@executable_path/../Frameworks/libglib.framework/Versions/2.0.0/libglib"
            -change "${PURPLE_LIBRARY_DIRS}/libgobject-2.0.0.dylib" "@executable_path/../Frameworks/libgobject.framework/Versions/2.0.0/libgobject"
            -change "/opt/homebrew/opt/gettext/lib/libintl.8.dylib" "@executable_path/../Frameworks/libintl.framework/Versions/8/libintl"
            -change "/opt/homebrew/opt/gdk-pixbuf/lib/libgdk_pixbuf-2.0.0.dylib" "@executable_path/../Frameworks/libgdk_pixbuf-2.0.0.dylib"
            "$<TARGET_FILE:${TARGET_NAME}>"
        COMMAND codesign --force --sign - "$<TARGET_FILE:${TARGET_NAME}>"
        COMMENT "Pointing ${TARGET_NAME} at the app bundle's libpurple and glib, then signing")
endif()

if (WIN32)
    install(TARGETS ${TARGET_NAME} RUNTIME DESTINATION "${PURPLE_PLUGIN_DIR}")
else()
    install(TARGETS ${TARGET_NAME} DESTINATION "${PURPLE_PLUGIN_DIR}")
endif()
