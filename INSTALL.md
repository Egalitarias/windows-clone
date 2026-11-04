# WinClone - Installation Prerequisites (macOS)

This guide installs everything needed to build and run the current project targets on macOS.

## 1) Install Homebrew (if needed)

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

## 2) Install required tools

```bash
brew install llvm qemu make
```

Notes:
- `llvm` provides `lld-link` for `make uefi-loader`
- `qemu` provides `qemu-system-x86_64` for `make qemu-run`
- `clang` usually comes from Apple Command Line Tools / Xcode tools

## 3) Ensure Apple command line tools exist

```bash
xcode-select -p || xcode-select --install
```

## 4) Verify tools

```bash
clang --version
make --version
qemu-system-x86_64 --version
"$(brew --prefix llvm)/bin/lld-link" --version
```

## 5) Build Week 1 targets

```bash
make clean
make all
```

## 6) Build UEFI loader (Week 2)

```bash
make uefi-loader UEFI_LLD_LINK="$(brew --prefix llvm)/bin/lld-link"
```

## 7) Locate OVMF firmware files for QEMU

The Makefile defaults (`/usr/share/OVMF/...`) are Linux-style paths. On macOS, pass explicit paths.

Find candidate firmware files:

```bash
find "$(brew --prefix)/share" -type f \( -name "*OVMF*fd" -o -name "edk2*x86_64*code*.fd" -o -name "edk2*x86_64*vars*.fd" \)
```

Then run QEMU with overrides:

```bash
make qemu-run \
  UEFI_LLD_LINK="$(brew --prefix llvm)/bin/lld-link" \
  OVMF_CODE="/path/to/code-firmware.fd" \
  OVMF_VARS_TEMPLATE="/path/to/vars-firmware.fd"
```

## Optional: avoid passing `UEFI_LLD_LINK` every time

For Intel Mac (Homebrew in `/usr/local`), add to `~/.zshrc`:

```bash
export PATH="/usr/local/opt/llvm/bin:$PATH"
```

For Apple Silicon Mac (Homebrew in `/opt/homebrew`):

```bash
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
```

Reload shell:

```bash
source ~/.zshrc
```

---

# Windows 11 (tested)

Everything below was verified on Windows 11 with the kernel booting in QEMU.

## 1) Install the toolchain with Scoop (no admin rights needed)

In PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm get.scoop.sh | iex"
scoop install llvm qemu make
```

`-ExecutionPolicy Bypass` applies to that single process only. Review the
installer at https://get.scoop.sh first if you prefer.

## 2) Build from Git Bash

The Makefile needs a POSIX shell (`uname`, `command -v`), so run `make` from
Git Bash, not PowerShell or cmd. Make's default `CC` is `cc`, so pass
`CC=clang`:

```bash
export PATH="$HOME/scoop/shims:$HOME/scoop/apps/llvm/current/bin:$HOME/scoop/apps/qemu/current:$PATH"
make CC=clang all
make CC=clang uefi-loader
```

`python3` must be on PATH (used to patch the kernel image size).

## 3) Boot in QEMU

QEMU for Windows ships edk2 firmware under `share/`, but the Makefile's OVMF
defaults are Linux/macOS paths. Use the helper script:

```bash
scripts/run-qemu-windows.sh            # serial output on this terminal
scripts/run-qemu-windows.sh --window   # open a QEMU window as well
```

Or pass the paths to `make` yourself:

```bash
Q="$HOME/scoop/apps/qemu/current/share"
make CC=clang qemu-run OVMF_CODE="$Q/edk2-x86_64-code.fd" OVMF_VARS_TEMPLATE="$Q/edk2-i386-vars.fd"
```
