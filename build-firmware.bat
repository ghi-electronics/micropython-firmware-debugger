@echo off
REM Build MicroPython firmware for a SITCore board on Windows.
REM
REM   build-firmware.bat                      build the default board
REM   build-firmware.bat SC13048Q             build a named board
REM   build-firmware.bat SC13048Q clean       delete that board's build output
REM   build-firmware.bat SC13048Q submodules  one-time submodule checkout
REM
REM When SC20xxx arrives, pass its board name as the first argument. Nothing
REM in this file is board-specific beyond that default.
REM
REM Optional environment overrides:
REM   ARM_GCC_BIN   bin directory of the arm-none-eabi toolchain, if it is not
REM                 already on PATH  (e.g. set ARM_GCC_BIN=C:\gcc\bin)
REM   GHI_LOADER    set to 1 to link behind the SITCore bootloader

setlocal enabledelayedexpansion
cd /d "%~dp0"

set "BOARD=%~1"
if "%BOARD%"=="" set "BOARD=SC13048Q"
set "ACTION=%~2"

REM ---------------------------------------------------------------- make ---
REM Git for Windows ships GNU Make 4.x, which the build needs for the linker
REM response file, plus the sh/sed/awk the makefiles call. Prefer whatever is
REM already on PATH, then fall back to the Git installation.
set "MAKE_EXE="
for /f "delims=" %%i in ('where make 2^>nul') do if not defined MAKE_EXE set "MAKE_EXE=%%i"
if not defined MAKE_EXE (
    for /f "delims=" %%i in ('where git 2^>nul') do (
        if not defined MAKE_EXE (
            if exist "%%~dpi..\usr\bin\make.exe" set "MAKE_EXE=%%~dpi..\usr\bin\make.exe"
        )
    )
)
if not defined MAKE_EXE (
    echo ERROR: GNU Make was not found.
    echo Install Git for Windows ^(it ships make, sh and sed^), or put make on PATH.
    exit /b 1
)
REM The recursive $(MAKE) inside the build is unquoted, so a path with a space
REM would split the command -- hence MAKE=make below, and this on PATH.
for %%i in ("%MAKE_EXE%") do set "PATH=%%~dpi;%PATH%"

REM ------------------------------------------------------------ toolchain ---
where arm-none-eabi-gcc >nul 2>&1
if errorlevel 1 (
    if defined ARM_GCC_BIN (
        if exist "%ARM_GCC_BIN%\arm-none-eabi-gcc.exe" set "PATH=%ARM_GCC_BIN%;%PATH%"
    )
)
where arm-none-eabi-gcc >nul 2>&1
if errorlevel 1 (
    echo ERROR: arm-none-eabi-gcc was not found on PATH.
    echo Point this script at your toolchain and run it again:
    echo     set ARM_GCC_BIN=^<path to the toolchain^>\bin
    exit /b 1
)

REM ------------------------------------------------------------ mpy-cross ---
REM There is no host C compiler on this machine, so mpy-cross cannot be built
REM from source; the prebuilt wheel from PyPI is used instead. Passing
REM MICROPY_MPYCROSS on the command line also stops the build trying to compile
REM its own copy.
where python >nul 2>&1
if errorlevel 1 (
    echo ERROR: python was not found on PATH.
    exit /b 1
)
set "MPYX_TXT=%TEMP%\mpyx_%RANDOM%.txt"
python -c "import mpy_cross,sys;sys.stdout.write(mpy_cross.mpy_cross)" > "%MPYX_TXT%" 2>nul
set "MPYX="
if exist "%MPYX_TXT%" set /p MPYX=<"%MPYX_TXT%"
del "%MPYX_TXT%" >nul 2>&1
if not defined MPYX (
    echo ERROR: the mpy-cross wheel is not installed.
    echo Install the version matching this checkout's tag:
    echo     python -m pip install "mpy-cross==<version>"
    exit /b 1
)
REM Validate the path rather than trusting it. A bad MICROPY_MPYCROSS does not
REM fail loudly -- it silently drops all frozen modules, and the link then dies
REM with an unrelated-looking "undefined reference to mp_find_frozen_module".
if not exist "%MPYX%" (
    echo ERROR: mpy-cross reported a path that does not exist:
    echo     %MPYX%
    echo Reinstall the wheel: python -m pip install --force-reinstall mpy-cross
    exit /b 1
)

set "COMMON=-C ports/stm32 BOARD=%BOARD% MAKE=make"
if defined GHI_LOADER set "COMMON=%COMMON% GHI_LOADER=%GHI_LOADER%"

REM --------------------------------------------------------------- actions ---
if /i "%ACTION%"=="clean" (
    echo ==^> cleaning %BOARD%
    "%MAKE_EXE%" %COMMON% clean
    exit /b !errorlevel!
)
if /i "%ACTION%"=="submodules" (
    echo ==^> checking out submodules
    "%MAKE_EXE%" %COMMON% submodules
    exit /b !errorlevel!
)

set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=4"

echo ==^> building %BOARD% with %JOBS% jobs
echo     make      %MAKE_EXE%
echo     mpy-cross %MPYX%
echo.
"%MAKE_EXE%" %COMMON% MICROPY_MPYCROSS="%MPYX%" MICROPY_MPYCROSS_DEPENDENCY="%MPYX%" -j%JOBS%
if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    echo If the error mentions mp_find_frozen_module, the incremental state
    echo cannot recover on its own -- run this first, then build again:
    echo     %~nx0 %BOARD% clean
    exit /b 1
)

echo.
echo ==^> built ports\stm32\build-%BOARD%\
where arm-none-eabi-size >nul 2>&1
if not errorlevel 1 arm-none-eabi-size "ports/stm32/build-%BOARD%/firmware.elf"
echo.
echo   firmware.hex  carries its load address -- safest to flash
echo   firmware.bin  raw image, no address inside
echo   firmware.dfu  for DFU tools
exit /b 0
