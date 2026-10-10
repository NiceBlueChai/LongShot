@echo off
setlocal
set "QT_ROOT=D:\Qt\Qt5.12.9\5.12.9\msvc2017_64"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set "PATH=%QT_ROOT%\bin;%PATH%"
cmake -S "%~dp0." -B "%~dp0build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_ROOT%"
if errorlevel 1 exit /b 1
cmake --build "%~dp0build"
if errorlevel 1 exit /b 1
ctest --test-dir "%~dp0build" --output-on-failure
if errorlevel 1 exit /b 1
if not exist "%~dp0dist" mkdir "%~dp0dist"
copy /y "%~dp0build\LongShot.exe" "%~dp0dist\LongShot.exe" >nul
if errorlevel 1 exit /b 1
copy /y "%~dp0LICENSE" "%~dp0dist\LICENSE" >nul
if errorlevel 1 exit /b 1
"%QT_ROOT%\bin\windeployqt.exe" --release --no-translations --no-opengl-sw ^
    --no-system-d3d-compiler "%~dp0dist\LongShot.exe"
if errorlevel 1 exit /b 1
"%~dp0build\longshot_tests.exe" deploymentSmoke trayLifecycle -o "%~dp0build\deployment-results.txt,txt"
exit /b %errorlevel%
