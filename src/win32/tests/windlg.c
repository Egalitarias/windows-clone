// Dialogs and resources test: a dialog from a compiled .rc resource (DIALOGEX), a modal DialogBox
// loop with IsDialogMessage, LoadString, LoadMenu, LoadAccelerators, and a dialog built from an
// in-memory template. The kernel self-test types into the dialog and presses buttons; the exit
// code is a bit mask of the checks that failed (0 = all passed).
#include <windows.h>

static unsigned g_bits;
static int g_init_param, g_focus_ok;
static char g_text[32];
static int g_checked;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

static INT_PTR CALLBACK DlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_INITDIALOG:
      g_init_param = (int)lp;
      g_focus_ok = (HWND)wp == GetDlgItem(h, 101);
      SetDlgItemTextA(h, 101, "ab");
      return TRUE;
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK) {
        char t[16];
        GetDlgItemTextA(h, 103, t, sizeof(t));
        CHECK(10, streq(t, "Name"));
        GetDlgItemTextA(h, 101, g_text, sizeof(g_text));
        g_checked = IsDlgButtonChecked(h, 102);
        EndDialog(h, 7);
        return TRUE;
      }
      if (LOWORD(wp) == IDCANCEL) {
        EndDialog(h, 99);
        return TRUE;
      }
      return FALSE;
    default:
      return FALSE;
  }
}

// The same kind of dialog written by hand (non-extended DLGTEMPLATE): a static and a button.
static INT_PTR CALLBACK MemProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)lp;
  if (m == WM_INITDIALOG) {
    return TRUE;
  }
  if (m == WM_COMMAND && LOWORD(wp) == IDOK) {
    char t[16];
    GetDlgItemTextA(h, 50, t, sizeof(t));
    EndDialog(h, streq(t, "Hi") ? 21 : 22);
    return TRUE;
  }
  return FALSE;
}

static WORD g_tpl[64];

static int put_str(WORD *p, const char *s) {
  int n = 0;
  while (*s) p[n++] = (WORD)*s++;
  p[n++] = 0;
  return n;
}

static int build_template(void) {
  WORD *p = g_tpl;
  int n = 0;

  *(DWORD *)p = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;   // style
  *(DWORD *)(p + 2) = 0;
  p[4] = 2;                                                         // two controls
  p[5] = 0; p[6] = 0; p[7] = 100; p[8] = 40;
  n = 9;
  p[n++] = 0;                                                       // no menu
  p[n++] = 0;                                                       // default class
  n += put_str(p + n, "Memory");
  while (n & 1) p[n++] = 0;                                         // items are DWORD aligned
  *(DWORD *)(p + n) = WS_CHILD | WS_VISIBLE;                        // a static: "Hi"
  *(DWORD *)(p + n + 2) = 0;
  p[n + 4] = 10; p[n + 5] = 8; p[n + 6] = 40; p[n + 7] = 10; p[n + 8] = 50;
  n += 9;
  p[n++] = 0xFFFF; p[n++] = 0x82;
  n += put_str(p + n, "Hi");
  p[n++] = 0;
  while (n & 1) p[n++] = 0;
  *(DWORD *)(p + n) = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
  *(DWORD *)(p + n + 2) = 0;
  p[n + 4] = 10; p[n + 5] = 22; p[n + 6] = 40; p[n + 7] = 14; p[n + 8] = IDOK;
  n += 9;
  p[n++] = 0xFFFF; p[n++] = 0x80;
  n += put_str(p + n, "Go");
  p[n++] = 0;
  return n;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  char s[32];
  HMENU menu, file;
  HACCEL acc;
  INT_PTR r;
  RECT rc = {10, 10, 20, 20};

  (void)prev;
  (void)cmd;
  (void)show;
  CHECK(0, LoadStringA(inst, 1, s, sizeof(s)) == 12 && streq(s, "Hello string"));
  CHECK(1, LoadStringA(inst, 99, s, sizeof(s)) == 0);
  CHECK(2, FindResourceA(inst, MAKEINTRESOURCE(999), RT_DIALOG) == 0);
  menu = LoadMenuA(inst, MAKEINTRESOURCE(200));
  file = GetSubMenu(menu, 0);
  CHECK(3, menu != 0 && GetMenuItemCount(menu) == 2 && GetMenuItemCount(file) == 3 && GetMenuItemID(file, 0) == 201 && GetMenuItemID(file, 1) == 0 &&
               GetMenuItemID(file, 2) == 202);
  CHECK(4, GetMenuStringA(file, 2, s, sizeof(s), MF_BYPOSITION) == 5 && streq(s, "E&xit"));
  acc = LoadAcceleratorsA(inst, MAKEINTRESOURCE(300));
  CHECK(5, acc != 0);
  CHECK(6, LoadStringA(inst, 17, s, sizeof(s)) == 6 && streq(s, "Second"));        // another block of the string table
  MapDialogRect(0, &rc);
  CHECK(7, rc.left == 17 && rc.top == 15 && rc.right == 35 && rc.bottom == 30);
  CHECK(8, FindResourceA(inst, MAKEINTRESOURCE(100), RT_DIALOG) != 0 && SizeofResource(inst, FindResourceA(inst, MAKEINTRESOURCE(100), RT_DIALOG)) > 40);
  r = DialogBoxParamA(inst, MAKEINTRESOURCE(100), 0, DlgProc, 1234);
  CHECK(9, r == 7);
  CHECK(11, g_init_param == 1234 && g_focus_ok);
  CHECK(12, streq(g_text, "xyab"));
  CHECK(13, g_checked == BST_CHECKED);
  build_template();
  CHECK(14, DialogBoxIndirectParamA(inst, (LPCDLGTEMPLATEA)g_tpl, 0, MemProc, 0) == 21);
  return (int)g_bits;
}
