@echo off
setlocal
set "TLROOT=%~dp0"
if "%TLROOT:~-1%"=="\" set "TLROOT=%TLROOT:~0,-1%"
set "PATH=%TLROOT%\bin\windows;%SystemRoot%\System32"
set "TEXMFROOT=%TLROOT%"
set "TEXMFCNF=%TLROOT%\texmf-dist\web2c"
set "TEXFORMATS=%TLROOT%\texmf-var\web2c\xetex;%TLROOT%\texmf-var\web2c\xetex\"
set "XE_FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "XE_FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
set "FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
if exist "%TLROOT%\bin\windows\icu-data\icudt*l.dat" set "ICU_DATA=%TLROOT%\bin\windows\icu-data"
"%TLROOT%\bin\windows\xetexdaemon.exe" -fmt=xelatexdaemon --no-font-cache-refresh %*
