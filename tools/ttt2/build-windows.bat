@echo off
setlocal
chcp 65001 >nul
rem Builds the Tekken Tag Tournament 2 port from your own dump of the game (Windows x86-64).
rem usage: build-windows.bat ["path\to\extracted game folder"]
rem Setup-TTT2.bat (the graphical setup) runs this with TTT2_NONINTERACTIVE=1 (no pauses) and TTT2_WORK=<build folder>.
cd /d "%~dp0..\.."
set "REPO=%CD%"
if not "%REPO:~50,1%"=="" (
  echo Warning: "%REPO%" is a long path. Some compiler paths may exceed the Windows limit;
  echo if the build fails, move this folder to a short path such as C:\TTT2Recomp.
)
set "GAME=%~1"
if "%GAME%"=="" (
  echo Drag the extracted game folder ^(with code, content, meta^) into this window and press Enter:
  set /p "GAME="
)
set GAME=%GAME:"=%
if not exist "%GAME%\code" (
  echo "%GAME%" has no code folder: choose the extracted game folder.
  if not defined TTT2_NONINTERACTIVE pause
  exit /b 1
)
set "PY="
where py >nul 2>nul && py -3 -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && python -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>nul && set "PY=python"
if not defined PY (
  if not exist "%REPO%\.tools\python\python.exe" (
    echo Downloading Python 3.14 ^(embeddable, python.org^)...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ErrorActionPreference='Stop'; New-Item -ItemType Directory -Force '%REPO%\.tools' | Out-Null;" ^
      "$z='%REPO%\.tools\python.zip'; Invoke-WebRequest -UseBasicParsing 'https://www.python.org/ftp/python/3.14.8/python-3.14.8-embed-amd64.zip' -OutFile $z;" ^
      "if ((Get-FileHash $z -Algorithm SHA256).Hash -ne 'A93ABE456AB01BD96D7A085B3CDB6566B3063F4241360D114142FBDB07F0A310') { throw 'Python download: wrong SHA-256' };" ^
      "Expand-Archive -Force $z '%REPO%\.tools\python'; Remove-Item $z"
    if errorlevel 1 ( echo Python download failed. & (if not defined TTT2_NONINTERACTIVE pause) & exit /b 1 )
  )
  set "PY="%REPO%\.tools\python\python.exe""
)
set "WORK=%REPO%\build-ttt2"
if defined TTT2_WORK set "WORK=%TTT2_WORK%"
set PYTHONUNBUFFERED=1
set PYTHONIOENCODING=utf-8
%PY% "%REPO%\tools\ttt2\build_ttt2.py" --game "%GAME%" --work "%WORK%"
if errorlevel 1 ( echo. & echo Build failed: see the messages above. & (if not defined TTT2_NONINTERACTIVE pause) & exit /b 1 )
echo.
echo Done. Start "%WORK%\TTT2-port\run_TTT2.bat"
if not defined TTT2_NONINTERACTIVE pause
