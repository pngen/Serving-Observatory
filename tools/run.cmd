@echo off
rem Launches a command with the MSVC + CUDA + CMake toolchain on PATH.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if defined CUDA_PATH set "PATH=%CUDA_PATH%\bin;%PATH%"
%*
