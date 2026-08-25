@echo off
setlocal
set "TLROOT=%~dp0"
if "%TLROOT:~-1%"=="\" set "TLROOT=%TLROOT:~0,-1%"
set "PROFILE=%~1"
if "%PROFILE%"=="" set "PROFILE=%TLROOT%\..\gui\profiles\unicodemath"
if not exist "%PROFILE%\warmup.tex" exit /b 0
if not exist "%TLROOT%\texmf-dist\web2c\texmf.cnf" exit /b 0
mkdir "%TLROOT%\texmf-var\fonts\conf\conf.d" >nul 2>nul
mkdir "%TLROOT%\texmf-var\fonts\cache" >nul 2>nul
(
echo ^<?xml version="1.0"?^>
echo ^<!DOCTYPE fontconfig SYSTEM "fonts.dtd"^>
echo ^<fontconfig^>
echo   ^<dir^>C:/Windows/fonts^</dir^>
echo   ^<dir^>%TLROOT:\=/%/texmf-dist/fonts/opentype^</dir^>
echo   ^<dir^>%TLROOT:\=/%/texmf-dist/fonts/truetype^</dir^>
echo   ^<cachedir^>%TLROOT:\=/%/texmf-var/fonts/cache^</cachedir^>
echo   ^<include ignore_missing="yes"^>conf.d^</include^>
echo   ^<config^>^<rescan^>^<int^>30^</int^>^</rescan^>^</config^>
echo ^</fontconfig^>
) > "%TLROOT%\texmf-var\fonts\conf\fonts.conf"
(
echo ^<?xml version="1.0"?^>
echo ^<!DOCTYPE fontconfig SYSTEM "fonts.dtd"^>
echo ^<fontconfig^>^</fontconfig^>
) > "%TLROOT%\texmf-var\fonts\conf\conf.d\51-local.conf"
set "PATH=%TLROOT%\bin\windows;%SystemRoot%\System32"
set "TEXMFROOT=%TLROOT%"
set "TEXMFCNF=%TLROOT%\texmf-dist\web2c"
set "TEXFORMATS=%TLROOT%\texmf-var\web2c\xetex;%TLROOT%\texmf-var\web2c\xetex\"
set "XE_FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "XE_FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
set "FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
if exist "%TLROOT%\bin\windows\icu-data\icudt*l.dat" set "ICU_DATA=%TLROOT%\bin\windows\icu-data"
del /q "%PROFILE%\warmup.xdv" "%PROFILE%\warmup.log" "%PROFILE%\warmup.aux" >nul 2>nul
"%TLROOT%\bin\windows\xetexdaemon.exe" -fmt=xelatexdaemon -no-pdf -interaction=nonstopmode -halt-on-error -output-directory="%PROFILE%" "%PROFILE%\warmup.tex"
if errorlevel 1 exit /b %ERRORLEVEL%
if not exist "%PROFILE%\warmup.xdv" exit /b 3
exit /b 0
