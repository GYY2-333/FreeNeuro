@echo off
rem build BrainBay Release and deploy to ..\bin
set MSB="C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\MSBuild.exe"
cd /d E:\EE317-GYY\BrainBay\src
%MSB% brainBay.sln /t:Build /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v142 /v:m /nologo
echo BUILD_EXITCODE=%ERRORLEVEL%
if exist Release\BrainBay.exe (
  copy /Y Release\BrainBay.exe ..\bin\BrainBay.exe
  echo COPY_EXITCODE=%ERRORLEVEL%
)
