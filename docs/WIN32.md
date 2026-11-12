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
  `ADVAPI32.dll`, `SHELL32.dll` and `COMCTL32.dll` all resolve to that one DLL, and so do the C
  runtime's names: `api-ms-win-crt-*.dll`, `api-ms-win-core-*.dll`, `ucrtbase.dll`, `VCRUNTIME140.dll`
  (and `_1`) and `msvcrt.dll` (see "The C runtime" below). An import it
  does not export is bound to a stub that ends the program with exit code `0xC0000139`; the
  kernel log lists them (`[PE] unresolved import KERNEL32.dll!Name`) so a program that never
  calls the missing function still runs. Imports by ordinal, delay-load imports, forwarders and
  other DLLs are not supported;
- gives the program a 2 MiB **heap block** at `PE_HEAP_BASE` (the runtime's `HeapAlloc` and
  `VirtualAlloc` carve it up; the `peinfo` syscall tells it where it is), a 64 KiB stack, and
  starts it with a Windows-style frame (shadow space, return address): returning from the entry
  point exits with the return value;
- builds the **thread block**: the first four pages of the heap block hold a TEB (with the
  `StackBase`/`StackLimit`, `Self`, `ThreadLocalStoragePointer` and `ProcessEnvironmentBlock`
  fields, and a small PEB with `ImageBaseAddress`) and the program's **thread-local storage**.
  The kernel loads `IA32_GS_BASE` with the TEB address whenever the task is switched in
  (`TCB.gs_base`, `cpu_set_gs_base`), so `gs:[0x30]` and `gs:[0x58]` work as on Windows. The
  `.tls` section's template is copied, `_tls_index` is set to 0, and the program's TLS callbacks
  run before its entry point. Up to 12 KiB of TLS data fit; a program with more is refused;
- enters the program through the runtime's `__winclone_start`, which runs the TLS callbacks
  and then calls the real entry point (found with `peinfo`).

Not supported: hardware exceptions reaching `__try/__except` or `catch` (a page fault or a division
by zero still ends the process, see "Exceptions"), COM.

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
  `QueryPerformanceCounter`, `Sleep`, `GetSystemTime`, `GetLocalTime`); threads, locks and TLS slots (see "Threads"); string helpers (`lstr*`, `MultiByteToWideChar` for ASCII); `GetLastError`.
  Paths are translated: `C:\DIR\FILE.TXT` is `/DIR/FILE.TXT` on the default volume.
- **user32**: `RegisterClass[Ex]A`, `CreateWindowExA`, `ShowWindow`, `UpdateWindow`, `DestroyWindow`,
  `GetMessage`, `PeekMessage`, `TranslateMessage`, `DispatchMessage`, `PostMessage`, `SendMessage`,
  `PostQuitMessage`, `DefWindowProc`, `InvalidateRect`, `BeginPaint`/`EndPaint`, `GetDC`/`ReleaseDC`,
  `SetTimer`/`KillTimer`, `GetClientRect`, `MoveWindow`/`SetWindowPos`, `SetWindowText`,
  `Get/SetWindowLong(Ptr)`, `GetSystemMetrics`, `GetKeyState`, `MessageBoxA` (a window of its own with
  the standard button sets), `wsprintfA`; child windows and the dialog helpers (`GetDlgItem`,
  `Set/GetDlgItemText`, `Set/GetDlgItemInt`, `CheckDlgButton`, `CheckRadioButton`, `IsDialogMessage`,
  `EnumChildWindows`, `GetWindow`, `GetParent`, `SetFocus`, `SetCapture`, `ClientToScreen`...).
- **gdi32**: device contexts, solid brushes, pens, stock objects, `FillRect`, `FrameRect`, `Rectangle`,
  `Ellipse`, `MoveToEx`/`LineTo`, `Polyline`, `SetPixel`, `PatBlt`, `TextOutA`, `ExtTextOutA`, `DrawTextA`,
  `GetTextExtentPoint32A`, `GetTextMetricsA`, `SetTextColor`, `SetBkColor`, `SetBkMode`, `GetDeviceCaps`.

Windows are WinClone windows: a pixel surface per window, at most 800 x 600 client pixels, two
top-level windows per program, drawn with the window syscalls (text is the built-in 8x8 font). Input events become messages: a key press gives `WM_KEYDOWN`, `WM_CHAR` and
`WM_KEYUP` together (so `TranslateMessage` does nothing), the left button gives `WM_LBUTTONDOWN/UP`
and `WM_MOUSEMOVE` (moves only while the button is held), the wheel `WM_MOUSEWHEEL`, a resize `WM_SIZE`,
the close button `WM_CLOSE`. `WM_PAINT` is generated for invalidated windows; `BeginPaint` sends
`WM_ERASEBKGND` and the default procedure fills with the class brush.

Missing on purpose (for now): clipboard (beyond the edit control's), fonts other
than the built-in one, right and middle mouse buttons, wide-character (`W`) window functions
(only `DefWindowProcW`, `GetMessageW`, ...), real wall-clock time (time counts from 2026-01-01 at
boot), environment variables, and anything about sockets.

## Child windows and standard controls

`CreateWindowEx` with `WS_CHILD` makes a child window: up to 64 windows per program, in a tree with
a z-order (creation order). A child has no surface of its own. It draws on its top-level window's,
through a device context whose origin is the child's place in the window and whose clip rectangle is
its client area cut by its ancestors', so a control never draws outside itself and `ClientToScreen`,
`GetWindowRect`, `MapWindowPoints` work in the top-level window's client coordinates. Behaviour that
matches Windows:

- **Painting.** `WM_PAINT` goes to the first shown window that needs it, parents before children.
  When a window is painted its children are painted again over it (there is no `WS_CLIPCHILDREN`),
  so a parent that erases its background does not hide its controls. Hiding, moving or destroying a
  child repaints the parent behind it.
- **Mouse.** A click goes to the topmost visible, enabled child under the pointer, with coordinates
  of that child; `SetCapture` sends everything to one window until `ReleaseCapture`. Disabled
  windows get no mouse input.
- **Keyboard.** Keys go to the window with the focus (`SetFocus`, `WM_SETFOCUS`/`WM_KILLFOCUS`; a
  control takes the focus when clicked). Mouse and keyboard input is aimed when the message is
  taken from the queue, not when it arrives, so a click that moves the focus decides where the keys
  typed right after it go. `IsDialogMessage` does what dialogs do: Tab and Shift+Tab move between
  the controls with `WS_TABSTOP`, Enter presses the focused or the default push button, Escape sends
  `IDCANCEL`.
- **Notifications** go to the parent as `WM_COMMAND` (`MAKEWPARAM(id, code)`, the control's handle
  in `lParam`), and before painting a control sends `WM_CTLCOLORBTN/STATIC/EDIT/LISTBOX` to its
  parent, so applications choose the colors (the default procedure answers with the usual button
  face and window colors).

The classes are registered when the first window is created (`src/win32/controls.c`):

| class | styles and messages |
| --- | --- |
| `BUTTON` | push buttons (`BS_PUSHBUTTON`, `BS_DEFPUSHBUTTON`), check boxes (plain, auto, 3-state), radio buttons (plain and auto, with `WS_GROUP` groups), `BS_GROUPBOX`, `BS_PUSHLIKE`; `BM_GETCHECK/SETCHECK/GETSTATE/SETSTATE/CLICK`; `BN_CLICKED`; Space presses it |
| `STATIC` | `SS_LEFT/CENTER/RIGHT/SIMPLE/LEFTNOWORDWRAP` text with word wrap, `&` mnemonics stripped (`SS_NOPREFIX`), `SS_CENTERIMAGE`, black/gray/white rectangles and frames, etched lines and frames, `SS_NOTIFY` (`STN_CLICKED`/`STN_DBLCLK`) |
| `EDIT` | single line and `ES_MULTILINE`; caret, selection (keys with Shift, mouse drag, double click selects a word, Ctrl+A), Home/End/arrows/Delete/Backspace, Ctrl+C/X/V through the system clipboard, `ES_PASSWORD`, `ES_READONLY`, `ES_NUMBER`, `ES_UPPERCASE/LOWERCASE`, scrolling both ways, mouse wheel; `EM_GETSEL/SETSEL/REPLACESEL/LIMITTEXT/GETLINECOUNT/LINEFROMCHAR/LINEINDEX/LINELENGTH/GETLINE/SETREADONLY/GETMODIFY/SETMODIFY/SCROLLCARET/SETPASSWORDCHAR/LINESCROLL`, `WM_CUT/COPY/PASTE/CLEAR`; `EN_CHANGE`, `EN_SETFOCUS`, `EN_KILLFOCUS` |
| `LISTBOX` | single, multiple and extended selection, `LBS_SORT`, `LBS_NOTIFY` (`LBN_SELCHANGE`, `LBN_DBLCLK`); a scroll bar (arrows, track, draggable thumb), wheel, Up/Down/PageUp/PageDown/Home/End, type-ahead; `LB_ADDSTRING/INSERTSTRING/DELETESTRING/RESETCONTENT/GETCOUNT/GETCURSEL/SETCURSEL/GETSEL/SETSEL/GETSELCOUNT/GETSELITEMS/GETTEXT/GETTEXTLEN/FINDSTRING/FINDSTRINGEXACT/SELECTSTRING/GETITEMDATA/SETITEMDATA/GETTOPINDEX/SETTOPINDEX/GETITEMRECT/ITEMFROMPOINT` |

| `COMBOBOX` | `CBS_SIMPLE`, `CBS_DROPDOWN` (an `EDIT` child, `CBN_EDITCHANGE`) and `CBS_DROPDOWNLIST`; `CBS_SORT`; Up/Down/PageUp/PageDown change the selection, F4 drops the list, Enter accepts and Escape cancels, type-ahead in the list box variety; `CB_ADDSTRING/INSERTSTRING/DELETESTRING/RESETCONTENT/GETCOUNT/GETCURSEL/SETCURSEL/GETLBTEXT/GETLBTEXTLEN/FINDSTRING/FINDSTRINGEXACT/SELECTSTRING/GETITEMDATA/SETITEMDATA/GETTOPINDEX/SETTOPINDEX/SHOWDROPDOWN/GETDROPPEDSTATE/LIMITTEXT/GETEDITSEL/SETEDITSEL/GETDROPPEDCONTROLRECT`; `CBN_SELCHANGE/DROPDOWN/CLOSEUP/SELENDOK/SELENDCANCEL/SETFOCUS/KILLFOCUS/DBLCLK` |
| `SCROLLBAR` | `SBS_HORZ`, `SBS_VERT`, the size box; arrow buttons, track and thumb (the thumb is proportional to `nPage`); sends `WM_HSCROLL`/`WM_VSCROLL` to the parent with `SB_LINEUP`, `SB_PAGEDOWN`, `SB_THUMBTRACK`, `SB_THUMBPOSITION`, `SB_ENDSCROLL` ...; the arrows and the track repeat while held; Up/Down/Left/Right, PageUp/PageDown, Home/End when focused; `SBM_*` messages |

A scroll bar control does not move itself: as on Windows it reports what the user did and the parent calls
`SetScrollPos` or `SetScrollInfo` (`SIF_RANGE`, `SIF_PAGE`, `SIF_POS`, `SIF_TRACKPOS`, `SIF_DISABLENOSCROLL`;
the largest position is `nMax - nPage + 1`). Also there: `GetScrollInfo`, `Set/GetScrollPos`, `Set/GetScrollRange`,
`EnableScrollBar` (the arrows can be disabled one at a time), `ShowScrollBar`.

**Scroll bars in a window's frame.** A window (top-level or child, of a class that is not one of the standard
controls) created with `WS_VSCROLL` and/or `WS_HSCROLL` gets a 17-pixel bar right of and/or below its client
area, with a grey corner where they meet; the client area is the rest (`GetClientRect`, `WM_SIZE` and drawing
use it, children are clipped to it; `GetWindowRect` and the size passed to `CreateWindow` / `MoveWindow` include
the bars). They are the same bars as the control: the program configures them with `SetScrollInfo` /
`SetScrollRange` / `SetScrollPos` using `SB_VERT` and `SB_HORZ`, and gets `WM_VSCROLL` / `WM_HSCROLL` itself
(with `lParam` 0) when the user clicks, holds, or drags. A bar with nothing to scroll shows disabled until
`SetScrollInfo` gives it a range; as on Windows, a call whose range makes the bar unnecessary hides it (unless
`SIF_DISABLENOSCROLL`) and `SetScrollInfo` with something to scroll shows it, so a window can get its bars
without the styles; `ShowScrollBar` shows and hides them (the window keeps its size; the client area gives
or takes the room and gets a `WM_SIZE`). The mouse on a bar never reaches the program as a client click; the
repeat timer is internal (the program sees no `WM_TIMER`). Not there: the non-client messages
(`WM_NCHITTEST`, `WM_NCLBUTTONDOWN`, `WM_NCPAINT`, `WM_NCCALCSIZE` are not sent; `DefWindowProc` does the work
inside the runtime), `SB_BOTH` for `SetScrollInfo`, `GetScrollBarInfo`, the mouse wheel and the keyboard (a window with bars does not react to them by itself).

**Scrolling the pixels.** `ScrollWindow`, `ScrollWindowEx` and `ScrollDC` move what is already drawn. For a
window the kernel copies the rectangle inside its surface (a new syscall, `win_copy`: the runtime cannot read a
surface back); what the move uncovers is invalidated (`ScrollWindow` always, `ScrollWindowEx` with
`SW_INVALIDATE` / `SW_ERASE`) and the program draws only that, since `BeginPaint` gives it the invalid rectangle
as `rcPaint` (and, as on Windows, drawing in `WM_PAINT`, including the background erase, is clipped to it). With no
rectangle, or `SW_SCROLLCHILDREN`, the child windows that touch the area move with it. `lprcScroll` and
`lprcClip` work as documented (the clip limits where pixels move); `lprcUpdate` returns the bounding box of what
was uncovered; a waiting invalid rectangle moves with the pixels. `ScrollDC` does the same on any DC, a memory
DC included, and invalidates nothing. Not there: regions (`hrgnUpdate` is ignored). A window keeps one invalid
rectangle, so two uncovered strips, or a strip while an earlier one is still unpainted, are merged into their
bounding box (correct, but more is repainted than on Windows).

A combo box follows Windows' layout rule: the height given to `CreateWindow` is the height of the drop-down
list plus the field, but the window is only the field's height (20 pixels) while closed, so it
does not cover what is under it; it grows while the list is down. The list is a `LISTBOX` child
inside the combo box, which holds the mouse capture while it is down and passes the mouse to the
list; clicking outside closes it. When it closes, the parent is painted again over the area.

Characters are 8 x 8 pixels, so a control sized for a proportional font shows less text than on
Windows; `WM_SETFONT` is remembered but changes nothing. A multi-line edit keeps `\n` internally and
hands out `\r\n`, and its selection positions count a line end as one character. Not there:
owner-drawn controls, tooltips, accelerators, undo, `WS_BORDER`/`WS_EX_CLIENTEDGE` for windows
that do not draw their own frame (controls always draw theirs), `SetParent`, an MDI client.

## Menus

`CreateMenu`/`CreatePopupMenu`, `AppendMenu`, `InsertMenu`, `ModifyMenu`, `DeleteMenu`, `RemoveMenu`,
`InsertMenuItem`/`GetMenuItemInfo`/`SetMenuItemInfo`, `GetMenuItemCount/ID`, `GetSubMenu`,
`GetMenuString`, `GetMenuState`, `CheckMenuItem`, `CheckMenuRadioItem`, `EnableMenuItem`, `IsMenu`,
`DestroyMenu`, `SetMenu`/`GetMenu`/`DrawMenuBar`, `TrackPopupMenu[Ex]` and accelerator tables
(`CreateAcceleratorTable`, `TranslateAccelerator`) work (`src/win32/menu.c`). A menu is passed to
`CreateWindowEx` (or `SetMenu`) as on Windows; a window's menu is destroyed with it.

- **The bar** takes the top 18 pixels of the window's surface; the client area starts below it
  (`GetClientRect`, drawing, mouse coordinates and `ClientToScreen` all account for it). The window
  size you ask for includes the bar, as on Windows.
- **Popups are not windows.** They are drawn over the client area while a modal loop reads the
  window's input; when one closes the window is painted again over the place it covered (your
  `WM_PAINT` runs). A popup is clamped to the surface, so in a small window a long menu is cut short.
- **Input:** hover and click, press-drag-release, Up/Down/Left/Right, Enter, Escape, a letter chooses
  the item with that `&` mnemonic (underlined), Alt+letter opens a bar item. Submenus nest 4 deep.
  Separators, check marks and radio bullets (`CheckMenuRadioItem`), disabled items (gray, not
  selectable), `\t` accelerator text right-aligned, an arrow for submenus.
- **Messages:** `WM_INITMENU` (bar) and `WM_INITMENUPOPUP` before a popup opens, so an application
  can enable and check items; choosing an item posts `WM_COMMAND` (`HIWORD` 0, or 1 from an
  accelerator; a disabled item's accelerator does nothing). `TrackPopupMenu` with `TPM_RETURNCMD`
  returns the command instead; `TPM_RIGHTALIGN`/`TPM_BOTTOMALIGN` are honored.

Not there: `LoadMenu` and menu resources, `WM_MENUSELECT`, owner-drawn and bitmap items, the system
menu, F10 and Alt alone for the bar (Alt+letter works), menu bar wrapping, scrolling of long popups,
`SetMenuDefaultItem`, `AppendMenuW` and the other `W` forms.

## Resources and dialogs

**Resources.** The program's `.rsrc` section is read in place (the whole image is mapped):
`FindResource[Ex]A`, `LoadResource`, `LockResource`, `SizeofResource`, `LoadStringA` (string
tables), `LoadMenuA`/`LoadMenuIndirectA` (`MENU` resources, and `lpszMenuName` of a window class
gives a window its menu), `LoadAcceleratorsA`. Build a `.rc` with `rc.exe` or `llvm-rc` and link the
`.res` (the test does: `windlg.rc`). Bitmap resources are read too (next section). Not there: `MENUEX`,
icons and cursors (`LoadIcon` and `LoadCursor` still return dummies), version info, the `W` forms.

**Dialogs.** `DialogBoxParam`, `DialogBoxIndirectParam`, `CreateDialogParam`,
`CreateDialogIndirectParam`, `EndDialog`, `DefDlgProc`, `GetDialogBaseUnits`, `MapDialogRect`, and
the item helpers from the controls section. A template is a `DLGTEMPLATE` or a `DLGTEMPLATEEX`
(what `DIALOG` and `DIALOGEX` compile to), from a resource or from memory. The dialog is a
top-level window of the class `#32770`; the controls are children created from the template
(classes by atom or by name, so custom classes work), the first `WS_TABSTOP` control gets the focus
and `WM_INITDIALOG` follows with the application's parameter, as on Windows. The dialog procedure's
return values follow the `DWLP_MSGRESULT` rules. `DialogBox` is modal: the owner is disabled and
a message loop with `IsDialogMessage` (Tab, Enter for the default button, Escape for `IDCANCEL`)
runs until `EndDialog`; the close button sends `IDCANCEL`.

Dialog units are converted with 7 x 12 pixels per 4 x 8 units (`GetDialogBaseUnits`), larger than
Windows' 8-point font, because characters here are 8 pixels wide. Not there: controls this runtime lacks
(common controls) in a template are skipped, `DS_` styles other than `DS_SETFONT` (the font is
ignored), dialog placement from the template (the window manager places it), `DM_SETDEFID`, system
modal and `MessageBox` owner disabling.

## Bitmaps

Bitmaps, memory DCs and the blit functions are in `bitmap.c`. A bitmap is a buffer of 32-bit
`0x00RRGGBB` pixels; a memory DC (`CreateCompatibleDC`, no window behind it) draws into the bitmap
selected into it, and everything in the GDI section works on one: `FillRect`, `TextOut`, `Rectangle`,
`Ellipse`, lines, `SetPixel`, `GetPixel`. A new memory DC holds a 1 x 1 monochrome bitmap, as on
Windows. Clipping to the bitmap is automatic. A bitmap cannot be deleted while it is selected into a
DC (`DeleteObject` fails, as on Windows).

- **Creating:** `CreateCompatibleBitmap` (always colour, unless the DC holds a monochrome bitmap),
  `CreateBitmap` (1 or 32 bits per pixel), `CreateBitmapIndirect`, `CreateDIBitmap`,
  `CreateDIBSection`, `LoadBitmapA` (a `BITMAP` resource, `.bmp` in the `.rc`), and `LoadImageA` with
  `IMAGE_BITMAP`, from a resource or with `LR_LOADFROMFILE` from a `.bmp` file. `GetObjectA`
  returns a `BITMAP` (a `DIBSECTION` for sections).
- **Blitting:** `BitBlt`, `StretchBlt` (nearest-neighbour; a negative width or height mirrors) and
  `PatBlt`, with every ternary raster operation (the 256 of them: the `SRCCOPY`, `SRCPAINT`,
  `SRCAND`, `SRCINVERT`, `NOTSRCCOPY`, `PATINVERT`, `DSTINVERT`, `BLACKNESS`, `WHITENESS` names and
  anything else a program builds with the ternary code; the pattern is the DC's solid brush). A
  monochrome source is drawn in the destination DC's text colour (0 bits) and background colour (1 bits).
- **DIBs:** `CreateDIBSection` for 32-bit sections (the program's pointer is the bitmap's memory, bottom-up
  or top-down); `SetDIBits`, `GetDIBits` (24 or 32-bit output), `StretchDIBits` and
  `SetDIBitsToDevice` read 1, 4, 8, 16 (5-5-5), 24 and 32-bit uncompressed DIBs with their colour tables.

A window's surface can be written (a blit goes to the kernel as a pixel copy) but not read back,
so a window DC works as the destination of operations that do not use the old pixels (`SRCCOPY`,
`NOTSRCCOPY`, `PATCOPY`, `BLACKNESS`, `WHITENESS`), not as a source, and `SRCINVERT`, `DSTINVERT`
and the like onto a window return `FALSE`; draw into a memory DC and blit that. `GetPixel` on a window DC
returns `CLR_INVALID`. A program that wants flicker-free drawing does the usual thing: paint into a
memory bitmap, then one `BitBlt` to the window in `WM_PAINT`.

Not there: `CreateDIBSection` for anything but 32 bits, `MaskBlt`, `PlgBlt`, `TransparentBlt` and
`AlphaBlend` (msimg32), `SetStretchBltMode` smoothing (always nearest), palettes, brush patterns
(`CreatePatternBrush`, `CreateDIBPatternBrush`), icons and cursors as images, device-dependent bitmap
bit access (`GetBitmapBits`).

Rectangle helpers (`SetRect`, `InflateRect`, `OffsetRect`, `IntersectRect`, `UnionRect`, `PtInRect` ...)
are in user32.

## The C runtime

Programs built the normal way, with the **dynamic C runtime (`/MD`)**, run unchanged: the
startup code the MSVC linker puts into the program (`mainCRTStartup` -> `__scrt_common_main_seh`,
from `msvcrt.lib`) calls the `ucrtbase` and `vcruntime140` functions it imports, and WIN32.DLL
provides about 400 of them (`src/win32/crt.c`, `crtfmt.c`, `crtmath.c`):

- **startup and exit**: `_configure_narrow_argv` (argv parsed from the command line, quotes and
  backslashes as Windows does), `_initterm`/`_initterm_e`, `_crt_atexit`/`atexit`, `exit`/`_exit`/
  `_cexit`, `abort`, the onexit tables, `signal`/`raise`, `_set_app_type` and friends; the
  security-cookie and `__scrt_*` code in the program itself needs only `QueryPerformanceCounter`,
  `InitializeSListHead`, `IsProcessorFeaturePresent` and similar, which `kernel32` now has;
- **memory**: `malloc`/`calloc`/`realloc`/`free`/`_msize`/`_aligned_malloc`, on the 2 MiB heap;
- **strings and characters**: the `str*`, `mem*`, `wcs*` families, `_stricmp`, `strtok_s`,
  `strcpy_s`..., `ctype` (`isalpha`, ... and the `__pctype_func` table the headers' macros use),
  `towlower`, `mbstowcs`;
- **numbers**: `strtol`/`strtoul`/`strtoll`/`strtod`/`atoi`/`atof`/`_itoa`..., with an exact
  `strtod` (hex floats, `inf`, `nan`, denormals) and an exact `printf` engine: `%d %i %u %x %X %o
  %c %s %p %f %e %g` with flags, width, precision, `*`, `hh h l ll z j t I64 I32 w`, wide strings
  (`%ls`, `%S`); doubles are printed from their exact decimal expansion and rounded to
  nearest-even. `sscanf`/`fscanf` handle `%d %i %u %x %o %c %s %[...] %f %n` and assignment
  suppression;
- **stdio**: `FILE` streams over the `CreateFileA` handles: `fopen`/`fopen_s` (`r w a`, `+`, `b`),
  `fread`/`fwrite`/`fgets`/`fputs`/`fgetc`/`ungetc`/`fseek`/`ftell`/`_fseeki64`/`fflush`/`fclose`,
  `remove`, `perror`; `stdin`/`stdout`/`stderr` on the console; the `__stdio_common_v*printf`/
  `scanf` entry points that the headers' inline `printf`, `snprintf`, `sscanf`... call;
- **math**: `sin cos tan asin acos atan atan2 sinh cosh tanh exp exp2 log log2 log10 pow sqrt
  cbrt hypot fmod floor ceil round trunc rint ldexp frexp modf fabs copysign fmin fmax`, the
  `float` versions, `_dclass`/`_fdclass`. The transcendental functions use the x87 unit with
  extended precision inside, so results are within about one unit in the last place (not always
  correctly rounded);
- **time**: `time`, `clock`, `gmtime`/`localtime` (UTC: there is no time zone), `mktime`,
  `strftime`, `asctime`; **misc**: `rand`/`srand` (the MSVC generator: `srand(1); rand()` is 41),
  `qsort`/`qsort_s`/`bsearch`, `div`, `setjmp`/`longjmp` (registers only: no unwinding),
  `_set_invalid_parameter_handler`, `getenv` (always NULL), `localeconv`, `setlocale` ("C").

Not there: `/MT` (static CRT) programs, which want hundreds of `kernel32` functions the runtime
does not have (`FlsAlloc` and `EncodePointer` are there, the locale, code page and string-type
functions are not); the C++ standard library (`msvcp140`: `std::string`, iostreams, `std::runtime_error`...;
`operator new`, `std::exception` and `std::bad_alloc` work); wide `printf` is narrowed to ASCII; text mode does not
translate `\r\n`; `fopen` takes the same translated paths as `CreateFileA`.

## Exceptions

C++ exceptions and structured exception handling work: `throw` and `catch` (by value, reference
or pointer, base classes, `catch (...)`, `throw;`), destructors run during unwinding, objects
that fail half way through construction are cleaned up, a `throw` inside a `catch` block, nested
`try` blocks inside `catch` blocks, exceptions in several threads at once, `noexcept` (an
exception leaving such a function ends the program), `std::exception` and `std::bad_alloc` from
`new`; and `__try`/`__except`/`__finally` with `RaiseException`, filters and
`GetExceptionInformation`. An exception nothing handles ends the program with a message and exit
code 3 (like `terminate`) for a C++ one, or the exception code for any other.

It is the real x64 mechanism, written from scratch in `src/win32/seh.c` and `cxxeh.c`:

- **Unwinder.** `RtlLookupFunctionEntry` finds the function's `RUNTIME_FUNCTION` in the `.pdata` of
  the program or of WIN32.DLL (the runtime is built with unwind tables so that exceptions can pass
  through its frames, for instance a window procedure it called), and `RtlVirtualUnwind` undoes
  the prolog described by the unwind info (pushes, stack allocations, frame pointer, saved
  registers, chained entries) to get the caller's registers, the function's establisher frame and
  its language handler. `RtlCaptureContext`/`RtlRestoreContext` save and load every register.
- **Dispatcher.** Raising an exception (`RaiseException`, `_CxxThrowException`, `RtlRaiseException`)
  captures the registers and runs two phases like Windows does. Phase 1 walks the stack calling each
  frame's language handler (`__C_specific_handler`, `__CxxFrameHandler3`) until one says it will
  handle the exception. Phase 2 walks it again with the unwinding flag so that handlers run the
  `__finally` blocks and destructors, and stops at the frame that handles it, where execution
  resumes: at the `__except` block, or at the address the C++ catch block returns.
- **C++ handler.** `__CxxFrameHandler3` reads the compiler's tables (`FuncInfo`: the unwind map,
  the try blocks and their handlers, the IP-to-state map; `ThrowInfo` and the catchable types of the
  thrown object, matched by type name). Searching, it finds the catch that matches; unwinding, it
  runs the actions of the unwind map from the state the frame was in down to the state of the
  catching try. Once everything below is unwound, the catch parameter is copied (copy constructor,
  `memcpy`, or a reference) and the catch block, a funclet, is called; the exception object is
  destroyed after it returns, unless a `throw;` handed it on.
- **Catch blocks run while the stack is still intact** (as on Windows), below the frames that were
  unwound, so the thrown object stays valid. An exception thrown from inside a catch block starts a
  new dispatch on top; walking up from there, the dead frames between the old throw and the catch
  block are skipped (the funclet returns to a marker address in `rt_call_catch`, where the walk
  continues in the function that owns the block), and a catch block that is left by an exception
  gives up its exception object. When the handler is a `try` inside a catch block, execution
  resumes inside that funclet; when it is outside, in the function that owns it.

The compiler's tables use the `__CxxFrameHandler3` layout, which clang emits. `cl.exe` since Visual
Studio 2019 compresses them (`__CxxFrameHandler4`); build with `/d2FH4-` for those, or the
runtime tells you and the exception goes unhandled. Not supported: **hardware exceptions** (a null
pointer, a division by zero, `int 3`) do not reach this code, because the kernel still ends the
process on a CPU fault; a filter returning `EXCEPTION_CONTINUE_EXECUTION` is treated as "keep
looking"; frames in the middle of a prolog or epilog (only an asynchronous fault can be there);
exceptions thrown by a destructor while another exception is unwinding; `std::exception_ptr`,
`std::current_exception`, `std::uncaught_exceptions`; `set_terminate` handlers.

## Threads

`CreateThread`, `_beginthreadex` and friends work: a program can run up to 16 threads (the main
one and 15 more), and the system as a whole 32 tasks (`SCHEDULER_MAX_TASKS`, which was 8).

**In the kernel** (`src/kernel/process.c`), an extra thread is one more scheduler task that runs in
the program's address space: `SYS_THREAD_CREATE(start_stub, fn, arg)` allocates a 64 KiB user
stack and a thread block (a TEB with its own copy of the TLS data, mapped next to the stack in
the process's page tables, slot `i` at `THREAD_VBASE + i * 128 KiB`), a kernel stack, and starts
a task whose function enters ring 3 at `start_stub` with `IA32_GS_BASE` pointing at the new TEB
(`TCB.gs_base`; the scheduler loads it at every switch). Per-task user-mode state that used to
be per-process (where an exit or fault returns to, "inside a syscall", the preempt level) lives
in `TCB.uctx`, so a fault in any thread's syscall unwinds that thread.

Ending a process ends all its threads. When the main thread returns, calls `exit`/`ExitProcess`,
or any thread faults or calls `exit`, the others are asked to end (`request_kill`): a task blocked
in a syscall is woken and its wait returns `-EINTR`, a task that was preempted in ring 3 has its
saved frame redirected to WIN32.DLL's `__winclone_die` (a bare `syscall` with `rax` = exit and
`rdi` = the exit code), and the syscall exit path checks the flag on the way out. `process_destroy`
then waits until every thread task has gone before it frees the address space, so nothing is left
running in freed memory. `ExitThread` ends only the calling thread (on the main thread it ends the
program); the thread's exit code is kept in user mode.

**Blocking** is one kernel primitive, a futex: `SYS_FUTEX_WAIT(addr, expected, timeout_ms)` blocks
while the 32-bit word at `addr` still holds `expected` (checked with interrupts off, so a wake-up
cannot be lost), `SYS_FUTEX_WAKE` wakes every waiter (they re-check, so this costs nothing but time).

**In WIN32.DLL** (`src/win32/threads.c`) everything else is built on that. All waiting goes through
one futex word that every state change increments: a waiter reads it, checks its condition, and
sleeps until the word changes or the timeout passes. This gives: thread, event (manual and
auto-reset), mutex (recursive, owner-checked) and semaphore handles with `WaitForSingleObject`,
`WaitForMultipleObjects` (any or all), `GetExitCodeThread`, `CREATE_SUSPENDED`/`ResumeThread`;
`CRITICAL_SECTION` (works as `InitializeCriticalSection` or zero-initialised),
`SRWLOCK`, `CONDITION_VARIABLE`, `InitOnceExecuteOnce`, `WaitOnAddress`; `Sleep`; `TlsAlloc` slots in
the TEB, so each thread has its own; `GetLastError` and `errno` per thread; `_beginthread[ex]`. The
runtime's own shared state is locked: the heap, the handle table, stdio streams, and the
float-printing buffers.

Not supported: `SuspendThread` and `TerminateThread` (fail), thread priorities and affinity (ignored),
fibers, thread pools, APCs and alertable waits, window messages across threads (create a window
and run its message loop on one thread), `strtok`/`gmtime`/`asctime` static buffers are shared
by all threads, `DLL_THREAD_ATTACH` callbacks.

## Building programs for it

Install Visual Studio with the C++ tools and a Windows 10/11 SDK (the build looks in the usual
places; set `WINSDK_ROOT`, `WINSDK_VER` and `MSVC_ROOT` if yours are elsewhere). Then:

```bash
make CC=clang uefi-loader     # builds WIN32.DLL and the test programs into out/uefi/APPS
```

Without the SDK that step is skipped with a note and the rest builds as before.

The simplest way is the Visual Studio way, with the real CRT headers and the dynamic C runtime:

```bash
clang --target=x86_64-pc-windows-msvc -O1 -fno-stack-protector -c app.c   # plus the SDK -isystem paths
lld-link -subsystem:console -entry:mainCRTStartup -nodefaultlib app.obj msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib
```

(`src/win32/tests/crttest.c` and the `crttest.exe` rule in the Makefile do exactly this; with
`cl /MD` or `clang-cl /MD` it is the same thing.) A program may also be linked with no C runtime
at all, which keeps it tiny; it then supplies what the compiler expects itself:

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
- `WINCXX.EXE` (`tests/cxxtest.cpp`, built by clang as C++ with `/MD`): 30 checks of exceptions: throws
  of `int`, classes, base and derived types, pointers and doubles through frames with destructors
  (the order of constructor and destructor calls is checked exactly), catch by value and
  reference, `std::exception`, rethrow, a throw inside a catch block, try blocks inside catch
  blocks, half-constructed objects, 3000 throws in a loop, `new` throwing `std::bad_alloc`,
  throwing in three threads at once, `__try/__except/__finally` with filters and parameters, a C++
  exception through a `__finally`. With `uncaught` and `noexcept` arguments the program ends with
  exit code 3;
- `WINTHR.EXE` (`tests/thrtest.c`, `/MD`): 45 checks of threads: join and exit codes, a critical
  section and interlocked counters over four threads (80000 increments each way), auto-reset and
  manual events, a bounded producer/consumer with a mutex and semaphores, per-thread TLS, last
  error and `errno`, four threads hammering `malloc`/`sprintf` with floats, SRW lock + condition
  variable + `InitOnce`, timeouts and suspended start, 40 threads in a row. Run with an argument
  it ends in other ways with four threads spinning or waiting: the main thread returns (exit code
  3), a thread calls `exit(5)`, a thread writes to address 0 (exit code 270);
- `WINCRT.EXE` (`tests/crttest.c`, built with `/MD`): 119 checks of the C runtime: `printf` (integers,
  exact doubles, widths, wide strings), `sscanf`, string functions, `strtod`/`strtol`, math,
  `malloc`/`qsort`/`bsearch`, file I/O round trips, `gmtime`/`strftime`/`mktime`, `setjmp`/`longjmp`,
  `__declspec(thread)` variables, `atexit`; the exit code is the number of failed checks (0);
- `WINBAD.EXE`: calls an import the runtime lacks (ends with `0xC0000139`); a file with only an
  `MZ` header is refused;
- `WINFP.EXE` three at once: long floating-point computations that must repeat exactly while the
  others run (fails if the SSE state is not saved per task: checked by disabling the save);
- `WINMSG.EXE`: `MessageBoxA`; Enter returns `IDYES`;
- `WINCTL.EXE` (`tests/winctl.c`): the standard controls. The kernel clicks, types, presses Tab,
  Space and Enter and double-clicks; pixels of every control and a custom child window are checked
  (class brush, STATIC, EDIT, BUTTON, LISTBOX with its scroll bar, device context origin), and the
  program checks 32 things (dialog item API, notifications, radio groups, focus, `IsDialogMessage`,
  disabled windows, `ES_NUMBER`, multi-line edit, child coordinates) and exits with a bit mask of the
  ones that failed (0 = all passed);
- `WINMENU.EXE` (`tests/winmenu.c`): menus. The kernel clicks the bar, hovers, chooses items, uses
  the keyboard in a submenu, Alt+F, Escape, Ctrl+N and a popup from `TrackPopupMenu`; pixels show
  the bar, a popup over the client area and the client painted again; the program checks 19 things
  (menu API, no command from a disabled item or Escape, client size under the bar) and exits with a
  bit mask of the failures;
- `WINCOMBO.EXE` (`tests/wincombo.c`): a list-box combo and an editable one. The kernel opens them
  with the mouse, chooses items, types, uses Down/Enter/Escape and type-ahead; pixels show the list
  over the parent and the parent painted again; 16 checks of notifications, text and selection;
- `WINSCROLL.EXE` (`tests/winscroll.c`): a vertical and a horizontal scroll bar. The kernel clicks the
  arrows and the track, drags the thumb, clicks a disabled arrow and uses Home/End/PageDown/Down/Up; pixels
  show the thumb at the bottom after the drag; the program counts the notifications and checks the
  scroll API (17 checks), exit code = bit mask of failures;
- `WINSBAR.EXE` (`tests/winsbar.c`): `WS_VSCROLL | WS_HSCROLL` on a top-level window and a child window. The kernel
  clicks the arrows and the track, drags the thumb, clicks the corner, the client area and the child's bar,
  hides the vertical bar with a key and shows it again; pixels show the bars, the thumb, the content
  moved by the program and the client area taking the room back; 15 checks, exit code = bit mask of failures;
- `WINSCRL.EXE` (`tests/winscrl.c`): colour bands and a black square that only a complete paint draws. The
  kernel scrolls the window with keys (`ScrollWindow` up and left, `ScrollWindowEx`); the program paints only the
  uncovered strip, so the pixels show whether they were really moved; `ScrollDC` on a bitmap and the reported
  update rectangles are checked in the program (10 checks);
- `WINBMP.EXE` (`tests/winbmp.c`, `winbmp.rc`, `pic.bmp`): memory DCs and bitmaps: raster operations,
  stretching and mirroring, DIB sections, `SetDIBits`/`GetDIBits`/`StretchDIBits`, a bitmap resource,
  text into a bitmap, a monochrome bitmap. 30 checks read pixels back from memory DCs; the kernel
  checks pixels of the blits onto the window (a resource scaled 20 times, mirrored, `NOTSRCCOPY`,
  `BLACKNESS`) and that a blit that would read the window draws nothing; exit code = bit mask of failures;
- `WINDLG.EXE` (`tests/windlg.c`, `windlg.rc`): a `DIALOGEX`, a menu, accelerators and a string table
  compiled into the program; a modal `DialogBoxParam` driven by the kernel (typing, a click, Tab,
  Space), a dialog built from an in-memory `DLGTEMPLATE`; 15 checks, exit code = bit mask of failures;
- `WINGUI.EXE`: window class, `WM_PAINT` with GDI (pixels are checked), mouse clicks, typed
  characters, a timer and `PostQuitMessage` (the exit code encodes what it saw);
- six more runs and a leak check (image, DLL and heap are freed).

`USER_SELFTEST=11` just starts `WINCTL.EXE` and leaves it on the desktop, for a
screenshot (`python scripts/qemu-screenshot.py out/run/shot.png 28`).

## Limits worth knowing

- At most 15 extra threads per program, 64 KiB of stack each (the main thread has 64 KiB too), 32 tasks on the whole system.
- The 2 MiB heap and the stacks are fixed; a program that needs more fails its allocations.
- Programs larger than about 900 KB (together with the runtime) do not fit the load window.
- Not a Windows compatibility layer in the sense of running arbitrary software: it runs programs
  that stay inside the subset above. Programs from real toolchains that link the CRT, use
  the C++ standard library, or use common controls will not get far yet.
