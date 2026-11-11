# Windows (PE) programs and the Win32 runtime

WinClone runs ordinary x64 Windows executables: programs built with the real Windows SDK
headers and import libraries, written against `kernel32`, `user32` and `gdi32`. A PE loader in the
kernel maps them, and a small Win32 runtime (`WIN32.DLL`, our own code) provides the API on
top of WinClone's own syscalls. No Windows code is involved; it is a re-implementation of a
subset of the Win32 API.

```
  program.exe  ──imports──▶  KERNEL32.dll / USER32.dll / GDI32.dll   (names the loader redirects)
        │                                   │
        │ PE loader (process.c)             ▼
        ▼                              WIN32.DLL  (src/win32: kernel32.c, user32.c, gdi32.c)
  one address space:                        │  WinClone syscalls (syscall.h): write, open, win_create, win_fill, ...
  exe, WIN32.DLL, heap, stack               ▼
                                          kernel
```

## The loader

`process_create` recognises a file starting with `MZ` and `PE\0\0` and, in `src/kernel/process.c`:

- accepts PE32+ (x86-64) **executables** for the console (3) or Windows (2) subsystem; DLLs, 32-bit
  files and .NET assemblies are refused (`PROCESS_ERR_BADPE`, the kernel log says which check failed);
- maps the headers and every section at `PROCESS_LOAD_BASE`, page by page (read-only for the
  headers, code and constants, writable where the section says so), and **applies the base
  relocations**, since the file's `ImageBase` is not where it lands. An image whose relocations
  were stripped must load at its `ImageBase`, which is not possible here and is refused;
- loads `/APPS/WIN32.DLL` into the same address space right behind it, relocates it, and **binds
  the program's imports by name** to its exports. `KERNEL32.dll`, `USER32.dll`, `GDI32.dll`,
  `ADVAPI32.dll`, `SHELL32.dll` and `COMCTL32.dll` all resolve to that one DLL. An import it
  does not export is bound to a stub that ends the program with exit code `0xC0000139`; the
  kernel log lists them (`[PE] unresolved import KERNEL32.dll!Name`) so a program that never
  calls the missing function still runs. Imports by ordinal, delay-load imports, forwarders and
  other DLLs are not supported;
- gives the program a 2 MiB **heap block** at `PE_HEAP_BASE` (the runtime's `HeapAlloc` and
  `VirtualAlloc` carve it up; the `peinfo` syscall tells it where it is), a 16 KiB stack, and
  starts it with a Windows-style frame (shadow space, return address): returning from the entry
  point exits with the return value.

Not supported: thread-local storage (a warning is logged), structured exception handling
(an exception ends the process like any other fault), threads (`CreateThread` fails), COM, the
real C runtime (see below).

Each program runs in its own address space as before; a PE program next to ELF programs is
fine. The kernel keeps each task's x87/SSE state apart (FXSAVE/FXRSTOR at every task switch,
`TCB.fx_state`), so programs may use floating point and SSE freely.

## What the runtime provides

About 230 functions, with exactly the SDK's prototypes (the runtime is compiled against the SDK
headers). Highlights:

- **kernel32**: `ExitProcess`, `GetCommandLineA/W`, `GetModuleHandle*`, `LoadLibrary`/`GetProcAddress`
  (for the runtime itself); the heap (`HeapAlloc/Free/ReAlloc/Size`, `GetProcessHeap`, `LocalAlloc`,
  `GlobalAlloc`, `VirtualAlloc/Free`); files and the console (`GetStdHandle`, `ReadFile`, `WriteFile`,
  `WriteConsole*`, `CreateFileA`, `CloseHandle`, `SetFilePointer`, `GetFileSize`, `DeleteFileA`,
  `CreateDirectoryA`, `GetFileAttributesA`, `FindFirstFileA`/`FindNextFileA`); time (`GetTickCount*`,
  `QueryPerformanceCounter`, `Sleep`, `GetSystemTime`, `GetLocalTime`); locks and TLS slots (single
  thread, so locks do nothing); string helpers (`lstr*`, `MultiByteToWideChar` for ASCII); `GetLastError`.
  Paths are translated: `C:\DIR\FILE.TXT` is `/DIR/FILE.TXT` on the default volume.
- **user32**: `RegisterClass[Ex]A`, `CreateWindowExA`, `ShowWindow`, `UpdateWindow`, `DestroyWindow`,
  `GetMessage`, `PeekMessage`, `TranslateMessage`, `DispatchMessage`, `PostMessage`, `SendMessage`,
  `PostQuitMessage`, `DefWindowProc`, `InvalidateRect`, `BeginPaint`/`EndPaint`, `GetDC`/`ReleaseDC`,
  `SetTimer`/`KillTimer`, `GetClientRect`, `MoveWindow`/`SetWindowPos`, `SetWindowText`,
  `Get/SetWindowLong(Ptr)`, `GetSystemMetrics`, `GetKeyState`, `MessageBoxA` (a window of its own with
  the standard button sets), `wsprintfA`.
- **gdi32**: device contexts, solid brushes, pens, stock objects, `FillRect`, `FrameRect`, `Rectangle`,
  `Ellipse`, `MoveToEx`/`LineTo`, `Polyline`, `SetPixel`, `PatBlt`, `TextOutA`, `ExtTextOutA`, `DrawTextA`,
  `GetTextExtentPoint32A`, `GetTextMetricsA`, `SetTextColor`, `SetBkColor`, `SetBkMode`, `GetDeviceCaps`.

Windows are WinClone windows: a pixel surface per window, at most 800 x 600 client pixels, two
windows per program, drawn with the window syscalls (so there is no clipping, and text is the
built-in 8x8 font). Input events become messages: a key press gives `WM_KEYDOWN`, `WM_CHAR` and
`WM_KEYUP` together (so `TranslateMessage` does nothing), the left button gives `WM_LBUTTONDOWN/UP`
and `WM_MOUSEMOVE` (moves only while the button is held), the wheel `WM_MOUSEWHEEL`, a resize `WM_SIZE`,
the close button `WM_CLOSE`. `WM_PAINT` is generated for invalidated windows; `BeginPaint` sends
`WM_ERASEBKGND` and the default procedure fills with the class brush.

Missing on purpose (for now): child windows and the standard controls (`BUTTON`, `EDIT`, ...), menus,
dialogs, memory DCs and bitmaps (`CreateCompatibleDC`, `BitBlt`), clipboard, resources, fonts other
than the built-in one, right and middle mouse buttons, wide-character (`W`) window functions
(only `DefWindowProcW`, `GetMessageW`, ...), real wall-clock time (time counts from 2026-01-01 at
boot), environment variables, and anything about threads or sockets.

## Building programs for it

Install Visual Studio with the C++ tools and a Windows 10/11 SDK (the build looks in the usual
places; set `WINSDK_ROOT`, `WINSDK_VER` and `MSVC_ROOT` if yours are elsewhere). Then:

```bash
make CC=clang uefi-loader     # builds WIN32.DLL and the test programs into out/uefi/APPS
```

Without the SDK that step is skipped with a note and the rest builds as before.

A program is compiled for `x86_64-pc-windows-msvc` and linked without the C runtime, since the
real CRT's startup needs far more of Windows than the runtime has:

```c
#include <windows.h>
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) { ... }
```

```bash
clang --target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -fno-stack-protector -c app.c  # plus the SDK -isystem paths
lld-link -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib app.obj crt.obj entry_gui.obj kernel32.lib user32.lib gdi32.lib
```

`src/win32/tests/crt.c` supplies what the compiler expects without a CRT (`memcpy`, `memset`,
`__chkstk`, `_fltused`) and `entry_console.c` / `entry_gui.c` the entry points (`mainCRTStartup` calls
`main`, `WinMainCRTStartup` calls `WinMain`). Copy the `.exe` into `\APPS` (with `WIN32.DLL`) on the
boot volume or any volume. Start it from Files (double-click) or the Console (`run NAME`, which
finds `NAME.BIN` or `NAME.EXE` in `\APPS`, or `run /PATH/NAME.EXE`).

## Testing

`make CC=clang USER_SELFTEST=10 uefi-loader`, boot, and read the `[PE]` lines on the serial log
(`scripts/run-qemu-windows.sh`, with `out/run/data.img` attached: the tests write files, and QEMU's
virtual FAT drive is not a good place for that). It runs, on the real desktop:

- `WINHELLO.EXE`: console output, the command line, the heap, a file round trip (exit code 7);
- `WINBAD.EXE`: calls an import the runtime lacks (ends with `0xC0000139`); a file with only an
  `MZ` header is refused;
- `WINFP.EXE` three at once: long floating-point computations that must repeat exactly while the
  others run (fails if the SSE state is not saved per task: checked by disabling the save);
- `WINMSG.EXE`: `MessageBoxA`; Enter returns `IDYES`;
- `WINGUI.EXE`: window class, `WM_PAINT` with GDI (pixels are checked), mouse clicks, typed
  characters, a timer and `PostQuitMessage` (the exit code encodes what it saw);
- six more runs and a leak check (image, DLL and heap are freed).

`USER_SELFTEST=11` just starts `WINGUI.EXE` and `WINMSG.EXE` and leaves them on the desktop, for a
screenshot (`python scripts/qemu-screenshot.py out/run/shot.png 28`).

## Limits worth knowing

- One thread. Anything that waits for another thread or process never wakes up.
- The 2 MiB heap and 16 KiB stack are fixed; a program that needs more fails its allocations.
- Programs larger than about 900 KB (together with the runtime) do not fit the load window.
- Not a Windows compatibility layer in the sense of running arbitrary software: it runs programs
  that stay inside the subset above. Programs from real toolchains that link the CRT, use
  exceptions or threads, or draw with bitmaps and controls will not get far yet.
