@echo off
rem Refresh qmllint module directory from generated qmltypes
set OUTDIR=%~1
if "%OUTDIR%"=="" exit /b 1
if not exist "%OUTDIR%\FwDelta" mkdir "%OUTDIR%\FwDelta"
copy /Y "%OUTDIR%\fw_delta.qmltypes" "%OUTDIR%\FwDelta\FwDelta.qmltypes" >nul
(echo module FwDelta& echo typeinfo FwDelta.qmltypes)> "%OUTDIR%\FwDelta\qmldir"
