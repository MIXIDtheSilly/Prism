@echo off
rem Prism's window (tools\prism_ui.pyw), without a console.
where pythonw >nul 2>nul && (start "" pythonw "%~dp0tools\prism_ui.pyw" %* & exit /b 0)
where pyw >nul 2>nul && (start "" pyw "%~dp0tools\prism_ui.pyw" %* & exit /b 0)
echo Prism needs Python 3.10 or later, and this PC doesn't have it (no pythonw or pyw on PATH).
echo Install it with: winget install Python.Python.3.13
echo (or from python.org, with "Add python.exe to PATH" ticked), then open Prism.cmd again.
pause
