@echo off

rem # author  # github.com\dkxce
rem # version # v1.0.0.0

:begin
chcp 65001 >nul
set "PROCESS_NAME=td9_aprs"

:iflaunched
tasklist /nh /fi "imagename eq %PROCESS_NAME%.exe" 2>nul | findstr /i "%PROCESS_NAME%" >nul
if %errorlevel% neq 0 goto build
echo Процесс %PROCESS_NAME% сейчас запущен.
set /p "choice=Остановить его и запустить сборку exe? [Y/N]: "
if /i "%choice%"=="Y" (
    powershell -Command "Stop-Process -Name %PROCESS_NAME% -Force"
	goto build
) else exit /b	

:build
@echo on
del td9_aprs.spec
.venv\Scripts\create-version-file td9_aprs.yaml --outfile td9_aprs.txt
.venv\Scripts\pyinstaller.exe --version-file="td9_aprs.txt" --name=td9_aprs --onefile td9_aprs.py
copy /Y test_exe_build.py td9_aprs.spec
.venv\Scripts\pyinstaller.exe --clean td9_aprs.spec
@del /Q td9_aprs.txt
tools\upx.exe -9f dist\td9_aprs.exe
move /y dist\td9_aprs.exe dist\td9_aprs.exe
@echo Сборка завершена
@pause

:end