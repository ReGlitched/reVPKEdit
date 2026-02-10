# Create executable
add_executable(${PROJECT_NAME}cli
        "${CMAKE_CURRENT_LIST_DIR}/Main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Tree.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/Tree.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPK.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPK.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPKPack.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPKPack.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPKManifest.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/RespawnVPKManifest.h")

vpkedit_configure_target(${PROJECT_NAME}cli)

# Rename the output binary without changing the internal CMake target name.
set_target_properties(${PROJECT_NAME}cli PROPERTIES OUTPUT_NAME "reVPKEditcli")

target_link_libraries(
        ${PROJECT_NAME}cli PUBLIC
        argparse::argparse
        indicators::indicators
        sourcepp::bsppp
        sourcepp::vpkpp)

if(TARGET lzham::bridge)
    target_link_libraries(${PROJECT_NAME}cli PRIVATE lzham::bridge)
    target_compile_definitions(${PROJECT_NAME}cli PRIVATE VPKEDIT_HAVE_LZHAM=1)

    add_custom_command(TARGET ${PROJECT_NAME}cli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:lzham_bridge>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}cli>")
endif()

target_include_directories(
        ${PROJECT_NAME}cli PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shared")
