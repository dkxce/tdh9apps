@echo off

rem # author  # github.com\dkxce
rem # version # v1.0.0.0

:begin
chcp 65001 >nul
set "PROCESS_NAME=td9_apps"

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
del td9_apps.spec
.venv\Scripts\create-version-file td9_apps.yaml --outfile td9_apps.txt
.venv\Scripts\pyinstaller.exe --version-file="td9_apps.txt" --name=td9_apps --onefile td9_apps.py
copy /Y test_exe_build.py td9_apps.spec
.venv\Scripts\pyinstaller.exe --clean td9_apps.spec
@del /Q td9_apps.txt
tools\upx.exe -9f dist\td9_apps.exe
move /y dist\td9_apps.exe dist\td9_apps.exe
@echo Сборка завершена
@pause

:end