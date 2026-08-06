@echo off
color 0A

echo =========================================
echo    [Protobuf] Compiling .proto files...
echo =========================================
echo.

REM Execute protoc compiler
protoc --cpp_out=. *.proto

REM Check execution result
if %errorlevel% neq 0 (
    color 0C
    echo.
    echo [ERROR] Compile Failed! 
    echo Please check your syntax or 'protoc' environment variable.
) else (
    echo [SUCCESS] C++ source files generated successfully!
)

echo.
echo =========================================
pause