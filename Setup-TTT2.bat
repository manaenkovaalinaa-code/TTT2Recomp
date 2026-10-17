@echo off
rem Tekken Tag Tournament 2 port: the graphical setup (build the game from your own copy, make a shortcut, play)
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0tools\ttt2\setup_gui.ps1"
