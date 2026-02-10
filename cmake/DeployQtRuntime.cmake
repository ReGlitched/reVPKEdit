# DeployQtRuntime.cmake
#
# CMake script-mode helper to copy the minimal Qt runtime next to an executable.
# This avoids needing to run windeployqt (which can fail due to permissions) and
# supports multi-config generators by using the built target's output dir.
#
# Required variables (passed via -D...):
# - QT_BASEDIR: Qt install root, e.g. C:/Qt/6.6.3/msvc2019_64
# - DEST_DIR:   directory containing the built exe (target file dir)
#
# Optional:
# - QT_LIB_SUFFIX: "d" for Debug, "" for Release (default: "")

if(NOT DEFINED QT_BASEDIR OR QT_BASEDIR STREQUAL "")
  message(FATAL_ERROR "DeployQtRuntime.cmake: QT_BASEDIR is required.")
endif()
if(NOT DEFINED DEST_DIR OR DEST_DIR STREQUAL "")
  message(FATAL_ERROR "DeployQtRuntime.cmake: DEST_DIR is required.")
endif()

if(NOT DEFINED QT_LIB_SUFFIX)
  set(QT_LIB_SUFFIX "")
endif()
set(_qt_raw "${QT_BASEDIR}")
string(REPLACE "\\\"" "\"" _qt_raw "${_qt_raw}")
string(REPLACE "\"" "" _qt_raw "${_qt_raw}")
file(TO_CMAKE_PATH "${_qt_raw}" QT_BASEDIR)

# MSBuild/Ninja can end up passing DEST_DIR with embedded quotes. Strip them so
# file(MAKE_DIRECTORY) doesn't try to create a relative folder like build/"C:/...".
set(_dest_raw "${DEST_DIR}")
string(REPLACE "\\\"" "\"" _dest_raw "${_dest_raw}")
# If the value contains a quoted absolute path (common with MSBuild custom commands),
# extract it before stripping remaining quotes.
string(REGEX MATCH "\"([A-Za-z]:[/\\\\][^\"]*)\"" _dest_quoted "${_dest_raw}")
if(_dest_quoted)
  string(REGEX REPLACE "^\"|\"$" "" _dest_raw "${_dest_quoted}")
endif()
string(REPLACE "\"" "" _dest_raw "${_dest_raw}")
file(TO_CMAKE_PATH "${_dest_raw}" DEST_DIR)

function(copy_file_if_exists SRC DST_DIR_IN)
  if(EXISTS "${SRC}")
    file(COPY "${SRC}" DESTINATION "${DST_DIR_IN}")
  endif()
endfunction()

function(copy_glob_if_any GLOB_EXPR DST_DIR_IN)
  file(GLOB _files "${GLOB_EXPR}")
  if(_files)
    file(COPY ${_files} DESTINATION "${DST_DIR_IN}")
  endif()
endfunction()

# Core Qt DLLs
copy_file_if_exists("${QT_BASEDIR}/bin/opengl32sw.dll" "${DEST_DIR}")
copy_file_if_exists("${QT_BASEDIR}/bin/Qt6Core${QT_LIB_SUFFIX}.dll" "${DEST_DIR}")
copy_file_if_exists("${QT_BASEDIR}/bin/Qt6Gui${QT_LIB_SUFFIX}.dll" "${DEST_DIR}")
copy_file_if_exists("${QT_BASEDIR}/bin/Qt6Widgets${QT_LIB_SUFFIX}.dll" "${DEST_DIR}")
copy_file_if_exists("${QT_BASEDIR}/bin/Qt6Network${QT_LIB_SUFFIX}.dll" "${DEST_DIR}")
copy_file_if_exists("${QT_BASEDIR}/bin/Qt6OpenGL${QT_LIB_SUFFIX}.dll" "${DEST_DIR}")
copy_file_if_exists("${QT_BASEDIR}/bin/Qt6OpenGLWidgets${QT_LIB_SUFFIX}.dll" "${DEST_DIR}")
copy_file_if_exists("${QT_BASEDIR}/bin/Qt6Svg${QT_LIB_SUFFIX}.dll" "${DEST_DIR}")

# Plugins
file(MAKE_DIRECTORY "${DEST_DIR}/platforms")
file(MAKE_DIRECTORY "${DEST_DIR}/styles")
file(MAKE_DIRECTORY "${DEST_DIR}/tls")

copy_file_if_exists("${QT_BASEDIR}/plugins/platforms/qwindows${QT_LIB_SUFFIX}.dll" "${DEST_DIR}/platforms")

if(EXISTS "${QT_BASEDIR}/plugins/styles/qmodernwindowsstyle${QT_LIB_SUFFIX}.dll")
  copy_file_if_exists("${QT_BASEDIR}/plugins/styles/qmodernwindowsstyle${QT_LIB_SUFFIX}.dll" "${DEST_DIR}/styles")
else()
  copy_file_if_exists("${QT_BASEDIR}/plugins/styles/qwindowsvistastyle${QT_LIB_SUFFIX}.dll" "${DEST_DIR}/styles")
endif()

copy_file_if_exists("${QT_BASEDIR}/plugins/tls/qcertonlybackend${QT_LIB_SUFFIX}.dll" "${DEST_DIR}/tls")
copy_file_if_exists("${QT_BASEDIR}/plugins/tls/qschannelbackend${QT_LIB_SUFFIX}.dll" "${DEST_DIR}/tls")
