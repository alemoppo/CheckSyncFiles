@echo off
rem ============================================================
rem  ATTIVA la profilazione MFT/hash e avvia la GUI.
rem
rem  Imposta BV_MFT_PROFILE (percorso base dei report). La GUI,
rem  lanciata da qui, a fine scansione in modalita' CONTENUTO
rem  scrive:
rem    - report MFT per drive : bin\mft_profile.<DRIVE>.txt
rem    - report hash/emit     : bin\mft_profile.hash.txt
rem  (la lettera drive e' solo un token nel NOME del file A/B, il
rem  contenuto resta sempre su questo disco).
rem
rem  IMPORTANTE: per catturare il tempo di backpressure devi
rem  mettere la modalita' "Contenuto" nella GUI prima di AVVIA.
rem ============================================================
setlocal
set "BV_MFT_PROFILE=%~dp0bin\mft_profile"
echo Profiler MFT/hash ATTIVO.
echo Report MFT per drive: %BV_MFT_PROFILE%.D.txt / .E.txt
echo Report hash/emit   : %BV_MFT_PROFILE%.hash.txt
echo Scegli la modalita' CONTENUTO nella GUI, poi premi AVVIA.
echo Avvio GUI...
start "" "%~dp0bin\bv_gui.exe"
endlocal