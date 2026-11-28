@echo off
setlocal

set "DASHBOARD_DIR=%~dp0dashboard"
set "PYTHON=%DASHBOARD_DIR%\.venv\Scripts\python.exe"

if not exist "%PYTHON%" (
    echo Chua tim thay moi truong Python cua dashboard.
    echo Hay chay cac lenh sau mot lan trong PowerShell:
    echo   cd /d "%DASHBOARD_DIR%"
    echo   python -m venv .venv
    echo   .\.venv\Scripts\python.exe -m pip install -r requirements-test.txt
    pause
    exit /b 1
)

"%PYTHON%" -c "import fastapi, uvicorn, paho.mqtt.client, amqtt" >nul 2>&1
if errorlevel 1 (
    echo Thieu goi phu thuoc cho dashboard.
    echo Hay chay lenh sau mot lan trong PowerShell:
    echo   cd /d "%DASHBOARD_DIR%"
    echo   .\.venv\Scripts\python.exe -m pip install -r requirements-test.txt
    pause
    exit /b 1
)

rem Reuse an existing dashboard so repeated clicks do not launch duplicates.
"%PYTHON%" -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/api/status', timeout=2).close()" >nul 2>&1
if not errorlevel 1 (
    echo Dashboard da chay. Dang mo lai trang web hien tai.
    start "" "http://127.0.0.1:8000"
    exit /b 0
)

if /I "%~1"=="--sim" (
    set "PEGATRON_DEVICE_ID=sorter-sim"
    set "PEGATRON_HARDWARE_CONTROL=0"
    set "PEGATRON_ESTOP_ABSENT=0"
    start "PEGATRON MQTT Broker" /D "%DASHBOARD_DIR%" "%PYTHON%" broker\dev_broker.py
    start "PEGATRON Dashboard" /D "%DASHBOARD_DIR%" "%PYTHON%" -m uvicorn app:app --host 127.0.0.1 --port 8000
    start "PEGATRON Simulator" /D "%DASHBOARD_DIR%" "%PYTHON%" simulator.py --interactive

    echo Dashboard dang chay o che do gia lap --sim. Khong dong ba cua so broker, dashboard va simulator.
    echo De dung, nhan Ctrl+C trong tung cua so.
) else (
    set "PEGATRON_DEVICE_ID=pico-01"
    set "PEGATRON_HARDWARE_CONTROL=1"
    set "PEGATRON_ESTOP_ABSENT=0"
    start "PEGATRON MQTT Broker (LAN)" /D "%DASHBOARD_DIR%" "%PYTHON%" broker\dev_broker.py --bind 0.0.0.0
    start "PEGATRON Dashboard (pico-01)" /D "%DASHBOARD_DIR%" "%PYTHON%" -m uvicorn app:app --host 0.0.0.0 --port 8000
    start "PEGATRON USB Console (COM6)" /D "%DASHBOARD_DIR%" "%PYTHON%" serial_bridge.py --logs-only --port COM6 --device pico-01

    echo Dashboard dang chay o che do phan cung that pico-01. USB Console su dung COM6 de thu log.
    echo Dong USB Console truoc khi su dung Serial Monitor hoac nap firmware. Doi COM6 trong file nay neu can.
)

rem Wait for the application, not an arbitrary startup delay.
"%PYTHON%" -c "import time,urllib.request; exec('for _ in range(30):\n try:\n  urllib.request.urlopen(\"http://127.0.0.1:8000/api/status\", timeout=1).close(); break\n except OSError: time.sleep(1)\nelse: raise SystemExit(1)')" >nul 2>&1
if errorlevel 1 (
    echo Khong mo duoc dashboard sau 30 giay. Kiem tra cua so PEGATRON Dashboard va cong 8000.
    pause
    exit /b 1
)
start "" "http://127.0.0.1:8000"
endlocal
