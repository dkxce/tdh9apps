rd /s /q  .\.prepare
md .\.prepare
md .\.prepare\gamespack
copy /y ..\myapps\8ballpool.h9mod .\.prepare\gamespack\
copy /y ..\myapps\aprspool.h9mod .\.prepare\gamespack\
copy /y ..\myapps\battlecity.h9mod .\.prepare\gamespack\
copy /y ..\myapps\bomberman.h9mod .\.prepare\gamespack\
copy /y ..\myapps\bomberman2.h9mod .\.prepare\gamespack\
copy /y ..\myapps\loadrunner.h9mod .\.prepare\gamespack\
copy /y ..\myapps\mineswiper.h9mod .\.prepare\gamespack\
copy /y ..\myapps\othello.h9mod .\.prepare\gamespack\
copy /y ..\myapps\seafight.h9mod .\.prepare\gamespack\
copy /y ..\myapps\snake.h9mod .\.prepare\gamespack\
copy /y ..\myapps\tetris.h9mod .\.prepare\gamespack\
folderhtml -smxX -t=".\files.txt" ".\.prepare"
"%ProgramFiles%\WinRAR\WinRAR.exe" a -afzip -r -ag-YYYYMMDD -m5 -ep1 -z".\.prepare\files.txt" "gamespack.zip" ".\.prepare\*"
rd /s /q  .\.prepare
pause