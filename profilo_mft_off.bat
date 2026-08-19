@echo off
rem ============================================================
rem  DISATTIVA la profilazione e avvia la GUI.
rem  Svuota BV_MFT_PROFILE: niente report MFT/hash extra.
rem ============================================================
setlocal
set "BV_MFT_PROFILE="
echo Profiler MFT/hash DISATTIVATO.
echo Avvio GUI...
start "" "%~dp0bin\bv_gui.exe"
endlocal