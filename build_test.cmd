@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist obj\test mkdir obj\test
if not exist bin mkdir bin
cl /nologo /std:c++17 /permissive- /O2 /EHsc /DNOMINMAX /W3 ^
   /I RSTuneTest /I RS_ASIO\RSTune /I RS_ASIO ^
   RSTuneTest\main.cpp ^
   RS_ASIO\RSTune\PitchShifter.cpp ^
   RS_ASIO\RSTune\PacketShifter.cpp ^
   RS_ASIO\AudioProcessing.cpp ^
   RS_ASIO\Utils.cpp ^
   RS_ASIO\Log.cpp ^
   /Fo:obj\test\ /Fe:bin\RSTuneTest.exe ^
   /link ole32.lib user32.lib propsys.lib
exit /b %errorlevel%
