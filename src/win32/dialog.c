// Resources and dialogs for the Win32 runtime: FindResource/LoadResource/LoadString/LoadMenu/
// LoadAccelerators read the .rsrc section of the program (the images are fully mapped), and
// DialogBox/CreateDialog build a dialog from a DLGTEMPLATE or DLGTEMPLATEEX, in memory or from a
// resource. A dialog is a top-level window of the class "#32770" whose controls are children made
// from the template; its procedure is the application's DLGPROC behind rt_dlg_proc. See docs/WIN32.md.
#include "internal.h"

// ---- Resources -----------------------------------------------------------------------------------------------

static const BYTE *module_base(HMODULE h) { return h != 0 ? (const BYTE *)h : (const BYTE *)(uintptr_t)rt_exe_base(); }

static const BYTE *rsrc_dir(const BYTE *base) {
  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
  const IMAGE_NT_HEADERS64 *nt;
  const IMAGE_DATA_DIRECTORY *d;

  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return 0;
  }
  nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE) {
    return 0;
  }
  d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
  return d->VirtualAddress != 0 ? base + d->VirtualAddress : 0;
}

static int ieq_wide(const WCHAR *w, int n, const char *s) {
  int i;

  for (i = 0; i < n; ++i) {
    char c = s[i], d = (char)w[i];

    if (c == 0 || w[i] > 127) return 0;
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (d >= 'A' && d <= 'Z') d = (char)(d + 32);
    if (c != d) return 0;
  }
  return s[n] == 0;
}

// The entry of directory `dir` called `name` (MAKEINTRESOURCE, a string, or "#number"); a null name matches the first.
static const IMAGE_RESOURCE_DIRECTORY_ENTRY *rsrc_entry(const BYTE *root, const IMAGE_RESOURCE_DIRECTORY *dir, ULONG_PTR name, int any) {
  const IMAGE_RESOURCE_DIRECTORY_ENTRY *e = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
  int i, n = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
  int is_str = (name >> 16) != 0;
  ULONG_PTR id = name;

  if (any) {
    return n > 0 ? e : 0;
  }
  if (is_str && ((const char *)name)[0] == '#') {                 // "#123" is the number 123
    const char *p = (const char *)name + 1;

    id = 0;
    while (*p >= '0' && *p <= '9') id = id * 10 + (ULONG_PTR)(*p++ - '0');
    is_str = 0;
  }
  for (i = 0; i < n; ++i) {
    if (is_str) {
      if (e[i].NameIsString) {
        const IMAGE_RESOURCE_DIR_STRING_U *s = (const IMAGE_RESOURCE_DIR_STRING_U *)(root + e[i].NameOffset);

        if (ieq_wide(s->NameString, s->Length, (const char *)name)) return &e[i];
      }
    } else if (!e[i].NameIsString && e[i].Id == (WORD)id) {
      return &e[i];
    }
  }
  return 0;
}

static const IMAGE_RESOURCE_DIRECTORY *sub_dir(const BYTE *root, const IMAGE_RESOURCE_DIRECTORY_ENTRY *e) {
  return e != 0 && e->DataIsDirectory ? (const IMAGE_RESOURCE_DIRECTORY *)(root + e->OffsetToDirectory) : 0;
}

EXPORT HRSRC WINAPI FindResourceExA(HMODULE hm, LPCSTR type, LPCSTR name, WORD lang) {
  const BYTE *base = module_base(hm), *root = rsrc_dir(base);
  const IMAGE_RESOURCE_DIRECTORY_ENTRY *e;
  const IMAGE_RESOURCE_DIRECTORY *d;

  (void)lang;
  if (root == 0 || (d = (const IMAGE_RESOURCE_DIRECTORY *)root, (e = rsrc_entry(root, d, (ULONG_PTR)type, 0)) == 0) ||
      (d = sub_dir(root, e)) == 0 || (e = rsrc_entry(root, d, (ULONG_PTR)name, 0)) == 0 || (d = sub_dir(root, e)) == 0 ||
      (e = rsrc_entry(root, d, 0, 1)) == 0 || e->DataIsDirectory) {
    SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
    return 0;
  }
  return (HRSRC)(root + e->OffsetToData);
}

EXPORT HRSRC WINAPI FindResourceA(HMODULE hm, LPCSTR name, LPCSTR type) { return FindResourceExA(hm, type, name, 0); }

EXPORT HGLOBAL WINAPI LoadResource(HMODULE hm, HRSRC r) {
  const IMAGE_RESOURCE_DATA_ENTRY *e = (const IMAGE_RESOURCE_DATA_ENTRY *)r;

  return e != 0 ? (HGLOBAL)(module_base(hm) + e->OffsetToData) : 0;
}

EXPORT LPVOID WINAPI LockResource(HGLOBAL g) { return g; }
EXPORT DWORD WINAPI SizeofResource(HMODULE hm, HRSRC r) { (void)hm; return r != 0 ? ((const IMAGE_RESOURCE_DATA_ENTRY *)r)->Size : 0; }
EXPORT BOOL WINAPI FreeResource(HGLOBAL g) { (void)g; return FALSE; }

static const void *res_data(HMODULE hm, LPCSTR name, LPCSTR type) {
  HRSRC r = FindResourceA(hm, name, type);

  return r != 0 ? LockResource(LoadResource(hm, r)) : 0;
}

// UTF-16 to the ASCII this runtime draws (anything above 127 becomes '?'); returns the characters and advances *pp past the NUL.
static int wide_to_ascii(const WORD **pp, char *out, int max) {
  const WORD *p = *pp;
  int n = 0;

  for (; *p != 0; ++p) {
    if (out != 0 && n < max - 1) out[n] = *p < 128 ? (char)*p : '?';
    ++n;
  }
  if (out != 0) out[n < max ? n : max - 1] = 0;
  *pp = p + 1;
  return n;
}

EXPORT int WINAPI LoadStringA(HINSTANCE hi, UINT id, LPSTR buf, int max) {
  const WORD *p = res_data(hi, (LPCSTR)(ULONG_PTR)((id >> 4) + 1), RT_STRING);
  UINT i;

  if (p == 0 || buf == 0 || max <= 0) {
    return 0;
  }
  for (i = 0; i < (id & 15); ++i) {
    p += 1 + *p;
  }
  if (*p == 0) {
    buf[0] = 0;
    return 0;
  }
  {
    int len = *p;
    WORD tmp[260];
    const WORD *q;
    int k;

    ++p;
    for (k = 0; k < len && k < 259; ++k) tmp[k] = p[k];
    tmp[k] = 0;
    q = tmp;
    return wide_to_ascii(&q, buf, max) > max - 1 ? max - 1 : (int)rt_strlen(buf);
  }
}

// ---- Menus and accelerators from resources -----------------------------------------------------------------

static const WORD *parse_menu(const WORD *p, HMENU m) {
  for (;;) {
    WORD opt = *p++, id = 0;
    HMENU sub = 0;
    char text[96];

    if (!(opt & MF_POPUP)) {
      id = *p++;
    }
    wide_to_ascii(&p, text, sizeof(text));
    if (opt & MF_POPUP) {
      sub = CreatePopupMenu();
      p = parse_menu(p, sub);
      AppendMenuA(m, MF_STRING | MF_POPUP | (opt & (MF_GRAYED | MF_DISABLED | MF_CHECKED)), (UINT_PTR)sub, text);
    } else if (opt == 0 && id == 0 && text[0] == 0) {
      AppendMenuA(m, MF_SEPARATOR, 0, 0);
    } else {
      AppendMenuA(m, MF_STRING | (opt & (MF_GRAYED | MF_DISABLED | MF_CHECKED)), id, text);
    }
    if (opt & MF_END) {
      return p;
    }
  }
}

EXPORT HMENU WINAPI LoadMenuIndirectA(const MENUTEMPLATEA *t) {
  const WORD *p = (const WORD *)t;
  HMENU m;

  if (p == 0 || p[0] != 0) {                                      // version 0 only (MENU, not MENUEX)
    return 0;
  }
  m = CreateMenu();
  if (m != 0) {
    parse_menu((const WORD *)((const BYTE *)p + 4 + p[1]), m);
  }
  return m;
}

EXPORT HMENU WINAPI LoadMenuA(HINSTANCE hi, LPCSTR name) {
  const void *d = res_data(hi, name, RT_MENU);

  return d != 0 ? LoadMenuIndirectA(d) : 0;
}

EXPORT HACCEL WINAPI LoadAcceleratorsA(HINSTANCE hi, LPCSTR name) {
  const BYTE *d = res_data(hi, name, RT_ACCELERATOR);
  ACCEL a[64];
  int n = 0;

  if (d == 0) {
    return 0;
  }
  for (;;) {
    BYTE fv = d[0];

    if (n < 64) {
      a[n].fVirt = fv & 0x7F;
      a[n].key = *(const WORD *)(d + 2);
      a[n].cmd = *(const WORD *)(d + 4);
      ++n;
    }
    d += 8;
    if (fv & 0x80) break;
  }
  return CreateAcceleratorTableA(a, n);
}

// ---- Dialogs -----------------------------------------------------------------------------------------------

// Dialog units: a character is taken to be 7 x 12 pixels (4 x 8 units), so a dialog designed for
// Windows' 8-point font has room for this runtime's 8 pixel wide characters.
#define DU_X(v) ((v) * 7 / 4)
#define DU_Y(v) ((v) * 12 / 8)
#define FRAME_W 16                                 // outer size minus client size of a top-level window (see user32.c)
#define FRAME_H 39

#define SLOT_DONE   24                             // GetWindowLong offsets of the dialog's own state
#define SLOT_RESULT 32
#define SLOT_MODAL  40

EXPORT LONG WINAPI GetDialogBaseUnits(void) { return MAKELONG(7, 12); }

EXPORT BOOL WINAPI MapDialogRect(HWND dlg, LPRECT r) {
  (void)dlg;
  r->left = DU_X(r->left);
  r->right = DU_X(r->right);
  r->top = DU_Y(r->top);
  r->bottom = DU_Y(r->bottom);
  return TRUE;
}

// A "sz or ordinal" field: 0 = none, 0xFFFF + id = ordinal, else a wide string.
static const WORD *read_sz_or_ord(const WORD *p, int *ord, char *text, int max) {
  *ord = -1;
  text[0] = 0;
  if (*p == 0) {
    return p + 1;
  }
  if (*p == 0xFFFF) {
    *ord = p[1];
    return p + 2;
  }
  wide_to_ascii(&p, text, max);
  return p;
}

static const char *atom_class(int ord) {
  switch (ord) {
    case 0x80: return "BUTTON";
    case 0x81: return "EDIT";
    case 0x82: return "STATIC";
    case 0x83: return "LISTBOX";
    case 0x84: return "SCROLLBAR";
    case 0x85: return "COMBOBOX";
    default: return "";
  }
}

static const BYTE *align4(const BYTE *start, const BYTE *p) { return start + (((size_t)(p - start) + 3u) & ~(size_t)3); }

static HWND build_dialog(HINSTANCE inst, const void *tpl, HWND owner, DLGPROC proc, LPARAM init) {
  const BYTE *start = (const BYTE *)tpl, *p;
  const WORD *w;
  int ex = ((const WORD *)tpl)[0] == 1 && ((const WORD *)tpl)[1] == 0xFFFF;
  DWORD style, exstyle;
  int count, x, y, cx, cy, ord, i;
  char title[96], cls[48];
  HMENU menu = 0;
  HWND dlg, first;
  LRESULT r;

  if (ex) {
    exstyle = *(const DWORD *)(start + 8);
    style = *(const DWORD *)(start + 12);
    count = *(const WORD *)(start + 16);
    x = *(const short *)(start + 18); y = *(const short *)(start + 20); cx = *(const short *)(start + 22); cy = *(const short *)(start + 24);
    w = (const WORD *)(start + 26);
  } else {
    style = *(const DWORD *)start;
    exstyle = *(const DWORD *)(start + 4);
    count = *(const WORD *)(start + 8);
    x = *(const short *)(start + 10); y = *(const short *)(start + 12); cx = *(const short *)(start + 14); cy = *(const short *)(start + 16);
    w = (const WORD *)(start + 18);
  }
  (void)x;
  (void)y;
  w = read_sz_or_ord(w, &ord, cls, sizeof(cls));                    // the menu
  if (ord >= 0) {
    menu = LoadMenuA(inst, (LPCSTR)(ULONG_PTR)ord);
  } else if (cls[0] != 0) {
    menu = LoadMenuA(inst, cls);
  }
  w = read_sz_or_ord(w, &ord, cls, sizeof(cls));                    // the window class (ignored: always "#32770")
  wide_to_ascii(&w, title, sizeof(title));
  if (style & DS_SETFONT) {
    w += ex ? 3 : 1;                                                // point size (, weight, italic and charset)
    {
      char face[32];

      wide_to_ascii(&w, face, sizeof(face));
    }
  }
  dlg = CreateWindowExA(exstyle, "#32770", title, style & ~(DWORD)(WS_VISIBLE | WS_CHILD), CW_USEDEFAULT, CW_USEDEFAULT,
                        DU_X(cx) + FRAME_W, DU_Y(cy) + FRAME_H + (menu != 0 ? RT_MENU_H : 0), owner, menu, inst, 0);
  if (dlg == 0) {
    if (menu != 0) DestroyMenu(menu);
    return 0;
  }
  SetWindowLongPtrA(dlg, DWLP_DLGPROC, (LONG_PTR)proc);
  p = (const BYTE *)w;
  for (i = 0; i < count; ++i) {
    DWORD istyle, iex;
    int ix, iy, icx, icy, id, tord;
    char ctext[128], cname[48];
    const char *cn;

    p = align4(start, p);
    if (ex) {
      iex = *(const DWORD *)(p + 4);
      istyle = *(const DWORD *)(p + 8);
      ix = *(const short *)(p + 12); iy = *(const short *)(p + 14); icx = *(const short *)(p + 16); icy = *(const short *)(p + 18);
      id = (int)*(const DWORD *)(p + 20);
      w = (const WORD *)(p + 24);
    } else {
      istyle = *(const DWORD *)p;
      iex = *(const DWORD *)(p + 4);
      ix = *(const short *)(p + 8); iy = *(const short *)(p + 10); icx = *(const short *)(p + 12); icy = *(const short *)(p + 14);
      id = *(const WORD *)(p + 16);
      w = (const WORD *)(p + 18);
    }
    w = read_sz_or_ord(w, &ord, cname, sizeof(cname));
    cn = ord >= 0 ? atom_class(ord) : cname;
    w = read_sz_or_ord(w, &tord, ctext, sizeof(ctext));
    p = (const BYTE *)w + 2 + *w;                                    // the creation data (skipped)
    CreateWindowExA(iex, cn, ctext, istyle | WS_CHILD, DU_X(ix), DU_Y(iy), DU_X(icx), DU_Y(icy), dlg, (HMENU)(UINT_PTR)id, inst, 0);
  }
  first = GetNextDlgTabItem(dlg, 0, FALSE);
  r = SendMessageA(dlg, WM_INITDIALOG, (WPARAM)first, init);
  if (r != 0 && first != 0) {
    SetFocus(first);
  }
  if (style & WS_VISIBLE) {
    ShowWindow(dlg, SW_SHOW);
  }
  return dlg;
}

LRESULT CALLBACK rt_dlg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  DLGPROC proc = (DLGPROC)GetWindowLongPtrA(h, DWLP_DLGPROC);

  if (proc != 0) {
    INT_PTR handled = proc(h, m, wp, lp);

    if (handled) {
      if (m == WM_INITDIALOG || (m >= WM_CTLCOLORMSGBOX && m <= WM_CTLCOLORSTATIC)) {
        return handled;
      }
      return GetWindowLongPtrA(h, DWLP_MSGRESULT);
    }
  }
  if (m == WM_CLOSE) {
    SendMessageA(h, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
    return 0;
  }
  if (m == WM_INITDIALOG) {
    return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

EXPORT LRESULT WINAPI DefDlgProcA(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_CLOSE) {
    SendMessageA(h, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
    return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

EXPORT HWND WINAPI CreateDialogIndirectParamA(HINSTANCE inst, LPCDLGTEMPLATEA t, HWND owner, DLGPROC proc, LPARAM init) {
  return build_dialog(inst, t, owner, proc, init);
}

EXPORT HWND WINAPI CreateDialogParamA(HINSTANCE inst, LPCSTR name, HWND owner, DLGPROC proc, LPARAM init) {
  const void *t = res_data(inst, name, RT_DIALOG);

  return t != 0 ? build_dialog(inst, t, owner, proc, init) : 0;
}

EXPORT BOOL WINAPI EndDialog(HWND h, INT_PTR result) {
  if (!IsWindow(h)) {
    return FALSE;
  }
  SetWindowLongPtrA(h, SLOT_RESULT, (LONG_PTR)result);
  SetWindowLongPtrA(h, SLOT_DONE, 1);
  if (!GetWindowLongPtrA(h, SLOT_MODAL)) {
    ShowWindow(h, SW_HIDE);
  }
  return TRUE;
}

EXPORT INT_PTR WINAPI DialogBoxIndirectParamA(HINSTANCE inst, LPCDLGTEMPLATEA t, HWND owner, DLGPROC proc, LPARAM init) {
  HWND dlg = build_dialog(inst, t, owner, proc, init);
  INT_PTR result;
  MSG msg;

  if (dlg == 0) {
    return -1;
  }
  SetWindowLongPtrA(dlg, SLOT_MODAL, 1);
  if (owner != 0) {
    EnableWindow(owner, FALSE);
  }
  ShowWindow(dlg, SW_SHOW);
  while (IsWindow(dlg) && !GetWindowLongPtrA(dlg, SLOT_DONE)) {
    BOOL r = GetMessageA(&msg, 0, 0, 0);

    if (r == 0) {                                                   // WM_QUIT: pass it on and leave
      PostQuitMessage((int)msg.wParam);
      SetWindowLongPtrA(dlg, SLOT_RESULT, 0);
      break;
    }
    if (!IsDialogMessageA(dlg, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }
  result = IsWindow(dlg) ? (INT_PTR)GetWindowLongPtrA(dlg, SLOT_RESULT) : 0;
  if (owner != 0) {
    EnableWindow(owner, TRUE);
  }
  DestroyWindow(dlg);
  return result;
}

EXPORT INT_PTR WINAPI DialogBoxParamA(HINSTANCE inst, LPCSTR name, HWND owner, DLGPROC proc, LPARAM init) {
  const void *t = res_data(inst, name, RT_DIALOG);

  return t != 0 ? DialogBoxIndirectParamA(inst, t, owner, proc, init) : -1;
}
