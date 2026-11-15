rd /s /q  .\.prepare
md .\.prepare
md .\.prepare\firmware
md .\.prepare\myapps
copy /y ..\dist\*.* .\.prepare
copy /y ..\custom_firmware\*.fw .\.prepare\firmware
copy /y ..\custom_firmware\*.exe .\.prepare\firmware
copy /y ..\myapps\*.h9mod .\.prepare\myapps
del .\.prepare\myapps\diaglock.h9mod
del .\.prepare\myapps\lockresearch.h9mod
folderhtml -smxX -t=".\files.txt" ".\.prepare"
type build_prepare.txt > ".\.prepare\files.tmp"
echo ``` >> ".\.prepare\files.tmp"
type ".\.prepare\files.txt" >> ".\.prepare\files.tmp"
echo ``` >> ".\.prepare\files.tmp"
move /y  ".\.prepare\files.tmp" ".\.prepare\files.txt"
"%ProgramFiles%\WinRAR\WinRAR.exe" a -afzip -r -ag-YYYYMMDD -m5 -ep1 -z".\.prepare\files.txt" "tdh9apps_v1.0.0.0.zip" ".\.prepare\*"
rd /s /q  .\.prepare
pause