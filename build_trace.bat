@echo off
REM Compatibility wrapper: build the current standalone proxy, not the old minimal tracer.
call "%~dp0build.bat" %*
exit /b %errorlevel%
