@echo off

git diff stub_base.s stub.s > stub.patch

setlocal enabledelayedexpansion
set "file=stub.patch"
set "tempfile=temp.txt"

< "!file!" (
    for /f "tokens=1,* delims=:" %%a in ('findstr /n "^^" "!file!"') do (
        set "line=%%b"
        if "%%a"=="3" (
            echo --- a/src/stub.s
        ) else if "%%a"=="4" (
            echo +++ b/src/stub.s
        ) else (
            echo(!line!
        )
    )
) > "!tempfile!"
move /y "!tempfile!" "!file!"
endlocal

echo READY