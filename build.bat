@echo off
rem Requires: python -m pip install ziglang
cd /d "%~dp0"
python -m ziglang cc -x c++ -target x86_64-windows-gnu -shared -O2 -std=c++17 -fno-exceptions -fno-rtti ^
    -Wall -Wno-unused-parameter ^
    -o BatteryCombo.dll BatteryCombo.cpp -lsetupapi -lgdi32 -luser32 -s
if errorlevel 1 exit /b 1
del /q BatteryCombo.lib BatteryCombo.pdb 2>nul
echo Built BatteryCombo.dll
