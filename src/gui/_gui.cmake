# Create executable
add_executable(${PROJECT_NAME} WIN32 MACOSX_BUNDLE
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/ControlsDialog.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/ControlsDialog.h"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/CreditsDialog.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/CreditsDialog.h"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/RevpkLogDialog.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/RevpkLogDialog.h"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/EntryOptionsDialog.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/EntryOptionsDialog.h"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/PackFileOptionsDialog.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/PackFileOptionsDialog.h"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/VerifyChecksumsDialog.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/VerifyChecksumsDialog.h"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/VerifySignatureDialog.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/dialogs/VerifySignatureDialog.h"

		"${CMAKE_CURRENT_LIST_DIR}/extensions/Folder.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/extensions/Folder.h"
		"${CMAKE_CURRENT_LIST_DIR}/extensions/SingleFile.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/extensions/SingleFile.h"

        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPK.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPK.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPKPack.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPKPack.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPKManifest.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPKManifest.h"

		"${CMAKE_CURRENT_LIST_DIR}/plugins/previews/IVPKEditPreviewPlugin.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/plugins/previews/IVPKEditPreviewPlugin.h"
		"${CMAKE_CURRENT_LIST_DIR}/plugins/previews/dmx/DMXPreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/plugins/previews/dmx/DMXPreview.h"
		"${CMAKE_CURRENT_LIST_DIR}/plugins/previews/mdl/MDLPreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/plugins/previews/mdl/MDLPreview.h"
		"${CMAKE_CURRENT_LIST_DIR}/plugins/previews/vcrypt/VCryptPreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/plugins/previews/vcrypt/VCryptPreview.h"
        "${CMAKE_CURRENT_LIST_DIR}/plugins/IVPKEditWindowAccess.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/plugins/IVPKEditWindowAccess.h"

		"${CMAKE_CURRENT_LIST_DIR}/previews/DirPreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/previews/DirPreview.h"
		"${CMAKE_CURRENT_LIST_DIR}/previews/EmptyPreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/previews/EmptyPreview.h"
		"${CMAKE_CURRENT_LIST_DIR}/previews/AudioPreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/previews/AudioPreview.h"
		"${CMAKE_CURRENT_LIST_DIR}/previews/InfoPreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/previews/InfoPreview.h"
		"${CMAKE_CURRENT_LIST_DIR}/previews/TextPreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/previews/TextPreview.h"
		"${CMAKE_CURRENT_LIST_DIR}/previews/TexturePreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/previews/TexturePreview.h"
		"${CMAKE_CURRENT_LIST_DIR}/previews/VPKFilePreview.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/previews/VPKFilePreview.h"

		"${CMAKE_CURRENT_SOURCE_DIR}/res/logo$<$<CONFIG:Debug>:_alt>.qrc"
		"${CMAKE_CURRENT_SOURCE_DIR}/res/res.qrc"

		"${CMAKE_CURRENT_LIST_DIR}/utility/DiscordPresence.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/utility/DiscordPresence.h"
		"${CMAKE_CURRENT_LIST_DIR}/utility/AudioPlayer.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/utility/AudioPlayer.h"
		"${CMAKE_CURRENT_LIST_DIR}/utility/ImageLoader.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/utility/ImageLoader.h"
		"${CMAKE_CURRENT_LIST_DIR}/utility/VTFConversion.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/utility/VTFConversion.h"
		"${CMAKE_CURRENT_LIST_DIR}/utility/Options.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/utility/Options.h"
		"${CMAKE_CURRENT_LIST_DIR}/utility/PluginFinder.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/utility/PluginFinder.h"
		"${CMAKE_CURRENT_LIST_DIR}/utility/TempDir.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/utility/TempDir.h"
        "${CMAKE_CURRENT_LIST_DIR}/utility/ThemedIcon.h"

		"${CMAKE_CURRENT_LIST_DIR}/EntryContextMenuData.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/EntryContextMenuData.h"
		"${CMAKE_CURRENT_LIST_DIR}/EntryTree.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/EntryTree.h"
		"${CMAKE_CURRENT_LIST_DIR}/FileViewer.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/FileViewer.h"
		"${CMAKE_CURRENT_LIST_DIR}/Main.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/Window.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/Window.h")

vpkedit_configure_target(${PROJECT_NAME})

# CMake's AUTOGEN (Qt moc/uic/rcc) spawns many processes by default. On some Windows setups this can fail
# with "libuv process spawn failed: operation not permitted". Keep it conservative.
set_property(TARGET ${PROJECT_NAME} PROPERTY AUTOGEN_PARALLEL 1)

# We compile the preview "plugins" into the main executable on Windows to avoid separate Qt plugin
# targets and their associated AUTOGEN spawn issues.
target_compile_definitions(${PROJECT_NAME} PRIVATE VPKEDIT_BUILTIN_PREVIEW_PLUGINS=1)

# Rename the output binary without changing the internal CMake target name.
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "reVPKEdit")

file(GLOB ${PROJECT_NAME}_I18N_TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/res/i18n/${PROJECT_NAME}_*.ts")
qt_add_translations(${PROJECT_NAME}
		TS_FILES ${${PROJECT_NAME}_I18N_TS_FILES}
		RESOURCE_PREFIX "/i18n"
		SOURCES ${${PROJECT_NAME}_SOURCES})

target_link_libraries(
        ${PROJECT_NAME} PRIVATE
        ${CMAKE_DL_LIBS}
        discord-rpc
        sourcepp::bsppp
        sourcepp::kvpp
        sourcepp::mdlpp
        sourcepp::steampp
        sourcepp::vcryptpp
        sourcepp::vpkpp
        sourcepp::vtfpp)

if(WIN32)
    # In-app audio preview via miniaudio, no Qt Multimedia dependency.
    # Link common Windows audio/COM libs (miniaudio uses runtime loading for some backends, but this is safe).
    target_link_libraries(${PROJECT_NAME} PRIVATE winmm ole32 avrt)
endif()

if(TARGET lzham::bridge)
    target_link_libraries(${PROJECT_NAME} PRIVATE lzham::bridge)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VPKEDIT_HAVE_LZHAM=1)

    # Ensure the wrapper DLL is next to the executable (needed at runtime).
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:lzham_bridge>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
endif()

target_include_directories(
        ${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared"
        "${CMAKE_CURRENT_SOURCE_DIR}/ext/gui/miniaudio")

target_use_qt(${PROJECT_NAME})

# Copy these next to the executable (important for stand-alone builds on Windows).
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/CREDITS.md"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/CREDITS.md")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/LICENSE")

# Add plugins
# Preview plugins are now built into the main executable (Windows-friendly: avoids separate Qt AUTOGEN targets).

if(LINUX)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VPKEDIT_LIBDIR="${CMAKE_INSTALL_LIBDIR}")
endif()
