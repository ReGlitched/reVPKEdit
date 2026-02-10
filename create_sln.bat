@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Qt install path:
rem - Prefer explicit arg %1
rem - Else use existing QT_BASEDIR env var (if set)
if not "%~1"=="" (
	set "QT_BASEDIR=%~1"
) else if "%QT_BASEDIR%"=="" (
	echo You must pass the path to your Qt installation as the first argument, or set QT_BASEDIR.
	echo Example:
	echo   %~nx0 "C:\Qt\6.6.3\msvc2019_64\"
	echo/
	pause
	exit /b 1
)

rem Strip trailing backslash to avoid malformed paths in CMake.
if "%QT_BASEDIR:~-1%"=="\" set "QT_BASEDIR=%QT_BASEDIR:~0,-1%"

echo Running CMake with Qt base directory set to "%QT_BASEDIR%"...
echo/
cmake -S . -B build -G "Visual Studio 17 2022" -DQT_BASEDIR="%QT_BASEDIR%"
set "CMAKE_RC=%ERRORLEVEL%"
echo/
if not "%CMAKE_RC%"=="0" (
    echo CMake failed with exit code %CMAKE_RC%.
    echo QT base directory was "%QT_BASEDIR%".
    echo/
    pause
    exit /b %CMAKE_RC%
)

echo Project sln is located at "build/vpkedit.sln".
echo/
pause
