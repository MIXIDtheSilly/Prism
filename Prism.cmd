@echo off
rem Prism's window (tools\prism_ui.pyw), without a console.
where pythonw >nul 2>nul && (start "" pythonw "%~dp0tools\prism_ui.pyw" %*) || (start "" pyw "%~dp0tools\prism_ui.pyw" %*)
