@echo off
cd /d D:\yingshe\daemon
echo ==========================================
echo   PetLink v1.2 - Claude Code + Codex CLI
echo   ASR: sherpa-onnx Zipformer Chinese 14M
echo ==========================================
echo.
echo Starting daemon...
node src\server.js
echo.
echo Daemon stopped. Press any key.
pause >nul
