@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ===== PetLink Asset Partition Burner =====
echo.

REM Check daemon is accessible
curl -s -m 2 http://127.0.0.1:7331/status >nul 2>&1
if !errorlevel! neq 0 (
    echo [WARN] daemon not running on 7331 — will try to flash anyway
) else (
    echo [OK] daemon found on 7331
    echo [STEP 1/3] Pausing daemon USB...
    curl -s -m 2 -X POST http://127.0.0.1:7331/usb/pause >nul
    echo [OK] USB paused
    timeout /t 2 /nobreak >nul
)

set ESP_IDF=D:\esp\v6.1\esp-idf
set PROJECT=D:\yingshe\04_firmware\esp32p4_firmware

echo [STEP 2/3] Burning asset partition...
python "%ESP_IDF%\components\partition_table\parttool.py" ^
    -p COM3 write_partition --partition-name asset ^
    --input "%PROJECT%\build\asset.img"
if !errorlevel! neq 0 (
    echo [ERROR] Asset burn failed
    goto cleanup
)
echo [OK] Asset partition burned

:cleanup
echo [STEP 3/3] Resuming daemon USB...
curl -s -m 2 -X POST http://127.0.0.1:7331/usb/resume >nul 2>&1
echo [OK] USB resumed

echo.
echo ===== Done =====
echo Reset the device to see the new assets.
