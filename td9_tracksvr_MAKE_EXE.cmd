@echo off

rem # author  # github.com\dkxce
rem # version # v1.0.0.0

:begin
chcp 65001 >nul
set "PROCESS_NAME=td9_tracksvr"

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
del td9_tracksvr.spec
.venv\Scripts\create-version-file td9_tracksvr.yaml --outfile td9_tracksvr.txt
.venv\Scripts\pyinstaller.exe --version-file="td9_tracksvr.txt" --name=td9_tracksvr --onefile td9_tracksvr.py
copy /Y test_exe_build.py td9_tracksvr.spec
.venv\Scripts\pyinstaller.exe --clean td9_tracksvr.spec
@del /Q td9_tracksvr.txt
tools\upx.exe -9f dist\td9_tracksvr.exe
move /y dist\td9_tracksvr.exe dist\td9_tracksvr.exe
@echo Сборка завершена
@pause

:end