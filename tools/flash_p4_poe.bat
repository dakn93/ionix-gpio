@echo off
setlocal

rem Ensure Git is available for pioarduino platform scripts.
set "PATH=%PATH%;C:\Program Files\Git\cmd"

rem Use short core dir to avoid Windows long-path extraction issues.
set "PLATFORMIO_CORE_DIR=C:\pio-core"

set "PYTHON_EXE=C:\Users\Daniel Knoll\AppData\Local\Programs\Python\Python312\python.exe"
if not exist "%PYTHON_EXE%" (
  echo Python not found at:
  echo %PYTHON_EXE%
  exit /b 1
)

echo [IONIX] Building and uploading waveshare_esp32_p4_poe...
"%PYTHON_EXE%" -m platformio run -e waveshare_esp32_p4_poe -t upload
exit /b %errorlevel%
