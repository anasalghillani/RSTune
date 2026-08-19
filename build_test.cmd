@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist obj\test mkdir obj\test
if not exist bin mkdir bin
cl /nologo /std:c++17 /O2 /EHsc /DNOMINMAX /W3 ^
   /I RSTuneTest /I RS_ASIO\RSTune ^
   RSTuneTest\main.cpp RS_ASIO\RSTune\PitchShifter.cpp ^
   /Fo:obj\test\ /Fe:bin\RSTuneTest.exe
exit /b %errorlevel%
