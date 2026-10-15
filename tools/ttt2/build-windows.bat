@echo off
setlocal
chcp 65001 >nul
rem Builds the Tekken Tag Tournament 2 port from your own dump of the game (Windows x86-64).
rem usage: build-windows.bat ["path\to\extracted game folder"]
cd /d "%~dp0..\.."
set "REPO=%CD%"
set "GAME=%~1"
if "%GAME%"=="" (
  echo Drag the extracted game folder ^(with code, content, meta^) into this window and press Enter:
  set /p "GAME="
)
set GAME=%GAME:"=%
if not exist "%GAME%\code" (
  echo "%GAME%" has no code folder: choose the extracted game folder.
  pause
  exit /b 1
)
set "PY="
where py >nul 2>nul && py -3 -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && python -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul && set "PY=python"
if not defined PY (
  if not exist "%REPO%\.tools\python\python.exe" (
    echo Downloading Python 3.14 ^(embeddable, python.org^)...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ErrorActionPreference='Stop'; New-Item -ItemType Directory -Force '%REPO%\.tools' | Out-Null;" ^
      "$z='%REPO%\.tools\python.zip'; Invoke-WebRequest -UseBasicParsing 'https://www.python.org/ftp/python/3.14.8/python-3.14.8-embed-amd64.zip' -OutFile $z;" ^
      "if ((Get-FileHash $z -Algorithm SHA256).Hash -ne 'A93ABE456AB01BD96D7A085B3CDB6566B3063F4241360D114142FBDB07F0A310') { throw 'Python download: wrong SHA-256' };" ^
      "Expand-Archive -Force $z '%REPO%\.tools\python'; Remove-Item $z"
    if errorlevel 1 ( echo Python download failed. & pause & exit /b 1 )
  )
  set "PY="%REPO%\.tools\python\python.exe""
)
%PY% "%REPO%\tools\ttt2\build_ttt2.py" --game "%GAME%"
if errorlevel 1 ( echo. & echo Build failed: see the messages above. & pause & exit /b 1 )
echo.
echo Done. Start "%REPO%\build-ttt2\TTT2-port\run_TTT2.bat"
pause
