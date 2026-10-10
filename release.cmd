@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set "QT_ROOT=D:\Qt\Qt5.12.9\5.12.9\msvc2017_64"
set "PATH=%QT_ROOT%\bin;%PATH%"
python "%~dp0tools\fetch_ocr.py"
if errorlevel 1 exit /b 1
cmake -S "%~dp0." -B "%~dp0build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_ROOT%"
if errorlevel 1 exit /b 1
cmake --build "%~dp0build"
if errorlevel 1 exit /b 1
python "%~dp0tools\package_release.py" --qt-root "%QT_ROOT%" --qt-source "D:\Qt\Qt5.12.9\5.12.9\Src"
exit /b %errorlevel%
