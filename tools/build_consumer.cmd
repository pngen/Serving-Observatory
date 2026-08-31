@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set SOBS_PREFIX=E:\The Journey\Coding\GitHub\production\Serving-Observatory\out\install
cmake -S cmake\consumer -B out\consumer-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%SOBS_PREFIX%"
if errorlevel 1 exit /b 1
cmake --build out\consumer-build
if errorlevel 1 exit /b 1
out\consumer-build\consumer.exe
