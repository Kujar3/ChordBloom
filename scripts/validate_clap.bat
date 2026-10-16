@echo off
rem Usage: scripts\validate_clap.bat [buildDir] [extra clap-validator args...]
rem   buildDir defaults to "build-clap". Validates <buildDir>\Chordbloom_artefacts\Release\CLAP\Chordbloom.clap
rem   with tools\clap-validator\clap-validator.exe (downloaded from the free-audio GitHub release if missing).
rem   Example: scripts\validate_clap.bat build-clap --only-failed
rem Expected (clap-validator 0.4.1): 0 failed. Skips are not-applicable tests (Unix-only scan, no preset
rem discovery, no 64-bit audio, no audio inputs, MIDI-dialect note input, no audio-port configs).
setlocal
set "ROOT=%~dp0.."
set "BUILD_DIR=%~1"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build-clap"
shift
set "EXTRA="
:collect
if "%~1"=="" goto done_collect
set "EXTRA=%EXTRA% %1"
shift
goto collect
:done_collect

set "VALIDATOR_DIR=%ROOT%\tools\clap-validator"
set "VALIDATOR=%VALIDATOR_DIR%\clap-validator.exe"
set "VALIDATOR_URL=https://github.com/free-audio/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-windows.zip"

if not exist "%VALIDATOR%" (
  echo Downloading clap-validator 0.4.1 ...
  if not exist "%VALIDATOR_DIR%\download" mkdir "%VALIDATOR_DIR%\download" || exit /b 1
  curl -sSL -o "%VALIDATOR_DIR%\download\clap-validator-windows.zip" "%VALIDATOR_URL%" || exit /b 1
  rem Windows' bsdtar (not Git's GNU tar, which treats "C:" as a remote host) extracts .zip files
  "%SystemRoot%\System32\tar.exe" -xf "%VALIDATOR_DIR%\download\clap-validator-windows.zip" -C "%VALIDATOR_DIR%" || exit /b 1
)

set "CLAP=%ROOT%\%BUILD_DIR%\Chordbloom_artefacts\Release\CLAP\Chordbloom.clap"
if not exist "%CLAP%" (
  echo %CLAP% not found. Build it first: scripts\build.bat %BUILD_DIR% Chordbloom_CLAP
  exit /b 1
)

"%VALIDATOR%" validate --hide-output%EXTRA% "%CLAP%"
exit /b %ERRORLEVEL%
