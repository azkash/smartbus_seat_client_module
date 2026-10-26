@echo off
title SMARTBUS Batch Auto-Flasher
cd /d "%~dp0"
echo =========================================================
echo    SMARTBUS PASSENGER SEAT BATCH AUTO-FLASHER
echo =========================================================
echo.
python -u flash_auto.py
echo.
pause
