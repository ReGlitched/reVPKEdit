# sourcepp
set(SOURCEPP_LIBS_START_ENABLED OFF CACHE INTERNAL "" FORCE)
set(SOURCEPP_USE_BSPPP          ON  CACHE INTERNAL "" FORCE)
set(SOURCEPP_USE_DMXPP          ON  CACHE INTERNAL "" FORCE)
set(SOURCEPP_USE_KVPP           ON  CACHE INTERNAL "" FORCE)
set(SOURCEPP_USE_MDLPP          ON  CACHE INTERNAL "" FORCE)
set(SOURCEPP_USE_STEAMPP        ON  CACHE INTERNAL "" FORCE)
set(SOURCEPP_USE_VCRYPTPP       ON  CACHE INTERNAL "" FORCE)
set(SOURCEPP_USE_VPKPP          ON  CACHE INTERNAL "" FORCE)
set(SOURCEPP_USE_VTFPP          ON  CACHE INTERNAL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/sourcepp")

# lzham (used for Respawn VPK compression)
# We vendor headers + a prebuilt lib from `.tmp/TFVPKTool-main` into `ext/shared/lzham`.
set(_VPKEDIT_LZHAM_HEADER "${CMAKE_CURRENT_LIST_DIR}/lzham/include/lzham.h")
set(_VPKEDIT_LZHAM_LIB    "${CMAKE_CURRENT_LIST_DIR}/lzham/lib/win64/lzham.lib")
# Make the build system re-run CMake if these appear/change after initial configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_VPKEDIT_LZHAM_HEADER}"
        "${_VPKEDIT_LZHAM_LIB}")

if(WIN32
        AND EXISTS "${_VPKEDIT_LZHAM_HEADER}"
        AND EXISTS "${_VPKEDIT_LZHAM_LIB}")
    add_library(lzham STATIC IMPORTED GLOBAL)
    add_library(lzham::lzham ALIAS lzham)
    set_target_properties(lzham PROPERTIES
            IMPORTED_LOCATION "${_VPKEDIT_LZHAM_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/lzham/include"
            INTERFACE_COMPILE_DEFINITIONS "VPKEDIT_HAVE_LZHAM=1")

    # Build a tiny wrapper DLL with /MT so the main Qt app can remain /MD without link mismatches.
    add_library(lzham_bridge SHARED
            "${CMAKE_CURRENT_LIST_DIR}/lzham_bridge/lzham_bridge.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lzham_bridge/lzham_bridge.h")
    add_library(lzham::bridge ALIAS lzham_bridge)
    target_include_directories(lzham_bridge
            PRIVATE "${CMAKE_CURRENT_LIST_DIR}/lzham/include"
            INTERFACE "${CMAKE_CURRENT_LIST_DIR}/lzham_bridge")
    target_link_libraries(lzham_bridge PRIVATE "${_VPKEDIT_LZHAM_LIB}")
    set_target_properties(lzham_bridge PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded"
            OUTPUT_NAME "lzham_bridge")
endif()
