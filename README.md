# MicroPython Firmware with Source-Level Debugger

**A [MicroPython](https://github.com/micropython/micropython) fork with real source-level debugging support.** Breakpoints, step, call stack, live variables — driven from VS Code, over a single USB cable, on real microcontroller hardware.

Pairs with the [MicroPython Debugger extension for VS Code](https://github.com/ghi-electronics/micropython-vsc-extension).

![Stopped on a breakpoint on a real board, showing locals, watch, call stack and program output](https://raw.githubusercontent.com/ghi-electronics/micropython-vsc-extension/main/images/screenshot.png)

## About this project

This fork adds:

- The `mpdebug` engine (`shared/mpdebug/`) — a debug protocol embedded in MicroPython.
- Dual-CDC USB support — one channel keeps the standard REPL, the other carries the debug protocol so a host tool can pause the program, set breakpoints and read the stack without disturbing normal `print()` output.
- Board configurations wired up for the boards we officially support (Raspberry Pi Pico, Pico 2, ESP32-S2, ESP32-S3).

**Most users don't need to build from source.** Install the [VS Code extension](https://github.com/ghi-electronics/micropython-vsc-extension) — pre-built firmware for common boards is shipped with it, installed on F5. See the extension's README for the current supported-board list.

This repository is for people who want to **build the firmware themselves for a board that isn't in that list**, or contribute changes to the debug engine.

## Building for a custom board

**Prerequisites** — the standard MicroPython toolchain for your target port. See [MicroPython's Getting Started](https://docs.micropython.org/en/latest/develop/gettingstarted.html):

| Port | Toolchain |
|---|---|
| **rp2** | CMake ≥ 3.13, GNU Make, `arm-none-eabi-gcc` **12 or newer**, `picotool` **2.3.0**, Python 3 |
| **esp32** | ESP-IDF **v5.5**, Python 3 (installed with ESP-IDF) |

This fork is based on **MicroPython v1.29.0** — the same version listed in `git describe` on the `dev` branch. Submodule pointers are pinned to what v1.29.0 references, so builds produce the tested toolchain output.

### Setting up the rp2 toolchain

- **`arm-none-eabi-gcc`** — install the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) 12.2 or newer. Older GCC (8.3, common on some machines under `C:\gcc`) fails at the link step because its `ld` doesn't parse the `--defsym` expressions the pico-sdk emits. `arm-none-eabi-gcc --version` should print 12.x or later.
- **`picotool` 2.3.0** — pico-sdk needs this to post-process the firmware. Grab a prebuilt binary from [pico-sdk-tools releases](https://github.com/raspberrypi/pico-sdk-tools/releases) and either put its folder on `PATH`, or set the `picotool_DIR` environment variable to the folder containing `picotoolConfig.cmake`. Without it, pico-sdk tries to build picotool from source, which is a fragile Windows path.

### Setting up ESP-IDF

ESP32 builds need ESP-IDF v5.5 installed and its environment sourced in every terminal session used for a build. Install it once via [Espressif's installer](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/windows-setup.html) on Windows or [the Linux / macOS instructions](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/linux-macos-setup.html), then load it before you build:

- **Windows** — launch the "ESP-IDF 5.5 PowerShell" (or "ESP-IDF 5.5 CMD") shortcut from your Start menu. It opens a shell with the env pre-loaded.
- **Linux / macOS** — source the export script in your shell:

  ```bash
  . $IDF_PATH/export.sh
  ```

After sourcing, verify with `Get-Command idf.py` on Windows PowerShell (should print a path) or `idf.py --version` on Linux / macOS. Only ESP32 builds need this; rp2 builds run from any regular shell.

**1. Clone this fork.**

```bash
git clone https://github.com/ghi-electronics/micropython-firmware-debugger.git
cd micropython-firmware-debugger
```

**2. Fetch the submodules for your port** (once):

```bash
make -C ports/<port> BOARD=<your-board> submodules
```

**3. Choose your board.**

If your hardware matches an upstream board in the tree (`RPI_PICO`, `RPI_PICO2`, `ESP32_GENERIC_S2`, `ESP32_GENERIC_S3`, etc.), just use that name — no board-file edits needed. The debugger engine and dual-CDC USB config live in the fork's shared code (`shared/mpdebug/`, `shared/tinyusb/`, `ports/<port>/mpdebug_port.c`), so every board built from this fork gets debug capability automatically.

If your hardware isn't in the tree, follow [MicroPython's board-adding guide](https://docs.micropython.org/en/latest/develop/porting.html) to create your own directory under `ports/<port>/boards/YOUR_BOARD/`. Copy the closest existing board and adjust pins.

**GHI's own release-build board configurations live in a separate repository**, [`micropython-firmware-debugger-ghiboards`](https://github.com/ghi-electronics/micropython-firmware-debugger-ghiboards), which is mounted here as a git submodule at `ghiboards/`. You do not need to init it — building any upstream board directly gives you working debug (breakpoints, step, stack, variables, deploy) with no auto-update prompts. The submodule only matters if you want to build GHI's exact release firmware, in which case run `git submodule update --init` and use `BOARD_DIR=ghiboards/<GHI_BOARD>` on the `make` command.

**4. Build.**

```bash
make -C ports/<port> BOARD=<your-board>
```

**5. Flash** the resulting `firmware.uf2` or `firmware.bin` to your board using the port's standard tool (`picotool`, `esptool.py`, etc.).

## Using your custom firmware with the extension

The VS Code extension auto-detects supported boards by USB VID/PID. It does not know yours, so pin the debug port manually in your project's `.vscode/launch.json`:

```jsonc
{
    "type": "micropython",
    "request": "launch",
    "name": "MicroPython Deploy and Debug (USB)",
    "program": "${workspaceFolder}/main.py",
    "debugPort": "COM4"       // or "/dev/ttyACM1" on Linux/macOS
}
```

`debugPort` is your board's **second** CDC — the debug channel. It is not the REPL. On Linux, check `ls /dev/serial/by-id/` — the entry ending in `-if02` is the debug channel.

Press **F5** in VS Code.

### Linux and macOS notes

**macOS** — nothing more to do. Serial devices are user-readable by default.

**Linux** — the extension ships udev rules only for the boards it officially supports. For your custom VID:PID, drop one line into `/etc/udev/rules.d/99-my-board.rules`, replacing `XXXX:YYYY` with your board's actual VID:PID (find with `lsusb`):

```
SUBSYSTEM=="tty", ATTRS{idVendor}=="XXXX", ATTRS{idProduct}=="YYYY", MODE="0666", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"
```

Then:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Replug the board. This solves two things at once: user-level access to `/dev/ttyACM*`, and telling ModemManager to leave the debug channel alone (otherwise its AT-command probing corrupts the first few seconds of the connection).

## Contributing

Bug reports and suggestions for the debugger are welcome — [open an issue](https://github.com/ghi-electronics/micropython-fw-debugger/issues).

For MicroPython core issues unrelated to debugging (interpreter, standard library, other ports), report those upstream at [micropython/micropython](https://github.com/micropython/micropython).

---

## About GHI Electronics

GHI Electronics is an embedded hardware and software company. We build the tools that make embedded development approachable — MicroPython here, and C# and .NET on our [TinyCLR](https://www.ghielectronics.com/tinyclr/) platform, which has been debugging production embedded devices for years. This project brings the same proven debugger protocol to MicroPython, so the source-level experience you expect on a desktop works on a small board too.

If you are new to GHI Electronics, take a look at our embedded devices and see where MicroPython fits alongside our C#/.NET platform:

| | |
|---|---|
| Website | [www.ghielectronics.com](https://www.ghielectronics.com) |
| Support | [support@ghielectronics.com](mailto:support@ghielectronics.com) |
| Forum | [forums.ghielectronics.com](https://forums.ghielectronics.com/) |
