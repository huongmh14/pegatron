@echo off
rem Build + nap firmware cho Pico 2 W. Bam dup file nay la xong.
rem   flash.bat           -> nap BangTaiPhanLoai (firmware chinh)
rem   flash.bat servo     -> nap TestServoTram1 (hieu chinh servo tram 1)
rem   flash.bat servo2    -> nap TestTwoServos (thu lan luot hai servo)
rem   flash.bat tof       -> nap TestToF
rem   flash.bat nobuild   -> chi nap lai UF2 da build
setlocal
cd /d "%~dp0"

set "SDK=%USERPROFILE%\.pico-sdk"
set "CMAKE=%SDK%\cmake\v4.3.4\bin\cmake.exe"
set "NINJA_DIR=%SDK%\ninja\v1.13.2"
set "PICOTOOL=%SDK%\picotool\2.3.1\picotool\picotool.exe"
if not exist "%PICOTOOL%" for /d %%D in ("%SDK%\picotool\*") do if exist "%%D\picotool\picotool.exe" set "PICOTOOL=%%D\picotool\picotool.exe"
set "OBJCOPY=%SDK%\toolchain\15_2_Rel1\bin\arm-none-eabi-objcopy.exe"
if not exist "%OBJCOPY%" for /d %%D in ("%SDK%\toolchain\*") do if exist "%%D\bin\arm-none-eabi-objcopy.exe" set "OBJCOPY=%%D\bin\arm-none-eabi-objcopy.exe"
set "PY=%~dp0dashboard\.venv\Scripts\python.exe"
if not exist "%PY%" set "PY=python"
set "PATH=%NINJA_DIR%;%PATH%"

set "TARGET=BangTaiPhanLoai"
set "BUILD=1"
if /i "%~1"=="servo"   set "TARGET=TestServoTram1"
if /i "%~1"=="servo2"  set "TARGET=TestTwoServos"
if /i "%~1"=="tof"     set "TARGET=TestToF"
if /i "%~1"=="relay"   set "TARGET=TestRelay"
if /i "%~1"=="nobuild" set "BUILD=0"

if /i "%BUILD%"=="0" goto :after_build

echo === BUILD %TARGET% ===
set "BEFORE_NS=0"
if exist "build\%TARGET%.elf" "%PY%" -c "import os; open(r'%TEMP%\pico_elf_ts.txt','w').write(str(os.stat(r'build/%TARGET%.elf').st_mtime_ns))" & set /p BEFORE_NS=<"%TEMP%\pico_elf_ts.txt"
"%CMAKE%" --build build --target %TARGET%
if %errorlevel% EQU 0 goto :after_build

if not exist "build\%TARGET%.elf" goto :build_err
"%PICOTOOL%" version >nul 2>&1
if %errorlevel% EQU 0 goto :build_err
"%PY%" -c "import os,sys; sys.exit(0 if os.stat(r'build/%TARGET%.elf').st_mtime_ns > int('%BEFORE_NS%') else 1)"
if not %errorlevel% EQU 0 goto :build_err

echo [!] picotool.exe bi Device Guard chan; dang dong goi UF2 bang arm-none-eabi-objcopy...
"%OBJCOPY%" -Obinary "build\%TARGET%.elf" "build\%TARGET%.bin"
if not %errorlevel% EQU 0 goto :build_err
"%PY%" -c "import struct; data=open(r'build/%TARGET%.bin','rb').read(); b0=struct.pack('<IIIIIIII',0x0A324655,0x9E5D5157,0x0000A000,0x10FFFF00,256,0,2,0xE48BFF57)+b'\xEF'*256+bytes.fromhex('04e35799')+b'\x00'*216+struct.pack('<I',0x0AB16F30); n=(len(data)+255)//256; uf2=b''.join([b0]+[struct.pack('<IIIIIIII',0x0A324655,0x9E5D5157,0x00002000,0x10000000+i*256,256,i,n,0xE48BFF59)+data[i*256:(i+1)*256].ljust(256,b'\x00')+b'\x00'*220+struct.pack('<I',0x0AB16F30) for i in range(n)]); open(r'build/%TARGET%.uf2','wb').write(uf2); print(f'UF2 OK: {len(uf2)} bytes ({len(uf2)//512} blocks)')"
if not %errorlevel% EQU 0 goto :build_err
goto :after_build

:build_err
echo.
echo *** BUILD LOI - khong nap.
pause
exit /b 1

:after_build
set "UF2=build\%TARGET%.uf2"
if not exist "%UF2%" ( echo Khong thay %UF2% & pause & exit /b 1 )

echo === NAP %UF2% ===
"%PICOTOOL%" version >nul 2>&1
if not %errorlevel% EQU 0 goto :bootsel_copy
rem Cach 1: tu reset Pico vao BOOTSEL qua USB (khong can bam nut) roi nap + chay lai.
"%PICOTOOL%" load -f -x "%UF2%"
if %errorlevel% EQU 0 ( echo. & echo === XONG === & timeout /t 2 >nul & exit /b 0 )

:bootsel_copy

rem Cach 2: Pico dang o BOOTSEL (o dia RPI-RP2 / RP2350) -> copy UF2.
echo Cach 1 that bai, thu tim o dia BOOTSEL...
for %%L in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist "%%L:\INFO_UF2.TXT" (
        copy /y "%UF2%" "%%L:\" >nul
        echo === XONG: da copy vao %%L: ===
        timeout /t 2 >nul
        exit /b 0
    )
)

echo.
echo *** Khong nap duoc. Giu nut BOOTSEL, cam USB (hoac bam RESET), roi chay lai file nay.
echo     Neu cong COM dang bi monitor/dashboard chiem, hay dong chung truoc.
pause
exit /b 1
