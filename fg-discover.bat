@echo off
rem Frame-generation discovery run: starts the KytyPS5 launcher with KYTY_FG_DISCOVER on.
rem Start Astro Bot from the launcher as usual and play Sky Garden for about 3 minutes,
rem moving the camera and Astro around. Then close the game.
rem The log, fg-discovery.txt, is written next to kyty_emulator.exe (the console shows its path).
set KYTY_FG_DISCOVER=1
set KYTY_FG_DISCOVER_START=3600
set KYTY_FG_DISCOVER_EVERY=900
set KYTY_FG_DISCOVER_COUNT=8
set KYTY_DRAW_RUN=0
cd /d "%~dp0"
start "" "%~dp0kyty_launcher.exe"
