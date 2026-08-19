@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
if not exist obj\app mkdir obj\app
if not exist bin mkdir bin
cl /nologo /std:c++17 /O2 /EHsc /W3 /DUNICODE /D_UNICODE ^
   /I RS_ASIO\RSTune ^
   RSTuneApp\main.cpp ^
   /Fo:obj\app\ /Fe:bin\RSTune.exe ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib
exit /b %errorlevel%
