@echo off
rem rebuild and run the flash probe (the interactive PowerShell session is unreliable)
cd /d "E:\EE317-GYY\BrainBay\tools"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /EHsc /MT flash_probe.cpp /I"C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Tools\MSVC\14.29.30133\atlmfc\include" /link /libpath:"C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Tools\MSVC\14.29.30133\atlmfc\lib\x86" atls.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK
del /q shot_*.bmp shot_*.png 2>nul
flash_probe.exe
echo PROBE EXITCODE=%ERRORLEVEL%
