// Menus test: a menu bar with popups, a submenu, a disabled item, a checked item, a separator,
// Alt+letter, an accelerator, and TrackPopupMenu with TPM_RETURNCMD. The kernel self-test injects
// mouse and key events (surface coordinates: the client area starts 18 pixels down); the exit code
// is a bit mask of the checks that failed (0 = all passed).
#include <windows.h>

#define ID_NEW   201
#define ID_OPEN  202
#define ID_SAVE  203
#define ID_EXIT  204
#define ID_CUT   301
#define ID_COPY  302
#define ID_DEEP  401
#define ID_ALPHA 501
#define ID_BETA  502

static unsigned g_bits;
static int g_new, g_open, g_save, g_deep, g_cmds, g_init, g_popup_cmd = -1;
static HMENU g_bar, g_popup;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

static void api_checks(HWND h) {
  HMENU file = GetSubMenu(g_bar, 0), edit = GetSubMenu(g_bar, 1), more = GetSubMenu(edit, 2);
  char t[32];
  RECT rc;
  POINT p = {0, 0};
  HMENU tmp = CreatePopupMenu();

  CHECK(0, GetMenu(h) == g_bar && IsMenu(g_bar) && !IsMenu((HMENU)12345));
  CHECK(1, GetMenuItemCount(g_bar) == 2 && GetMenuItemCount(file) == 5 && GetMenuItemCount(edit) == 3);
  CHECK(2, (GetMenuState(file, ID_SAVE, MF_BYCOMMAND) & MF_GRAYED) != 0 && (GetMenuState(file, ID_OPEN, MF_BYCOMMAND) & MF_GRAYED) == 0);
  CHECK(3, (GetMenuState(edit, ID_COPY, MF_BYCOMMAND) & MF_CHECKED) != 0);
  CHECK(4, more != 0 && GetMenuItemID(more, 0) == ID_DEEP && GetMenuItemID(edit, 2) == (UINT)-1);
  CHECK(5, GetMenuItemID(file, 3) == 0 && (GetMenuState(file, 3, MF_BYPOSITION) & MF_SEPARATOR) != 0);
  CHECK(6, GetMenuStringA(file, ID_NEW, t, sizeof(t), MF_BYCOMMAND) == 10 && streq(t, "New\tCtrl+N"));
  CHECK(7, CheckMenuItem(edit, ID_CUT, MF_CHECKED) == MF_UNCHECKED && CheckMenuItem(edit, ID_CUT, MF_UNCHECKED) == MF_CHECKED);
  CHECK(8, ID_DEEP == GetMenuItemID(GetSubMenu(g_bar, 1) ? more : 0, 0));
  AppendMenuA(tmp, MF_STRING, 1, "One");
  InsertMenuA(tmp, 0, MF_BYPOSITION | MF_STRING, 2, "Zero");
  CHECK(9, GetMenuItemCount(tmp) == 2 && GetMenuItemID(tmp, 0) == 2 && GetMenuItemID(tmp, 1) == 1);
  CHECK(10, DeleteMenu(tmp, 2, MF_BYCOMMAND) && GetMenuItemCount(tmp) == 1 && !DeleteMenu(tmp, 77, MF_BYCOMMAND));
  CHECK(11, EnableMenuItem(tmp, 1, MF_BYCOMMAND | MF_GRAYED) == 0 && (GetMenuState(tmp, 1, MF_BYCOMMAND) & MF_GRAYED) != 0);
  DestroyMenu(tmp);
  CHECK(12, !IsMenu(tmp));
  GetClientRect(h, &rc);
  CHECK(13, rc.right == 404 && rc.bottom == 243);                    // 420 x 300 outside, minus the frame and the 18-pixel menu bar
  ClientToScreen(h, &p);
  CHECK(14, p.x == 0 && p.y == 18);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      HBRUSH red = CreateSolidBrush(RGB(200, 40, 40));
      RECT r = {10, 10, 50, 40};
      FillRect(dc, &r, red);
      DeleteObject(red);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_INITMENUPOPUP:
      ++g_init;
      return 0;
    case WM_COMMAND:
      ++g_cmds;
      switch (LOWORD(wp)) {
        case ID_NEW: if (HIWORD(wp) == 1) ++g_new; break;      // 1 = from an accelerator
        case ID_OPEN: ++g_open; break;
        case ID_SAVE: ++g_save; break;
        case ID_DEEP: ++g_deep; break;
        default: break;
      }
      return 0;
    case WM_LBUTTONDOWN: {
      POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
      ClientToScreen(h, &p);
      g_popup_cmd = TrackPopupMenu(g_popup, TPM_RETURNCMD, p.x, p.y, 0, h, 0);
      return 0;
    }
    case WM_CLOSE:
      CHECK(15, g_open == 1 && g_save == 0 && g_deep == 1 && g_new == 1);
      CHECK(16, g_cmds == 3);                                  // nothing else, in particular not from Esc or the disabled item
      CHECK(17, g_popup_cmd == ID_BETA);
      CHECK(18, g_init >= 4);                                  // File, Edit, More, File again, the popup
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage((int)g_bits);
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  WNDCLASSA wc;
  HWND hwnd;
  MSG msg;
  HMENU file, edit, more;
  ACCEL acc[1];
  HACCEL ha;

  (void)prev;
  (void)cmd;
  ZeroMemory(&wc, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hbrBackground = CreateSolidBrush(RGB(200, 220, 255));
  wc.lpszClassName = "WinCloneMenus";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  g_bar = CreateMenu();
  file = CreatePopupMenu();
  edit = CreatePopupMenu();
  more = CreatePopupMenu();
  AppendMenuA(file, MF_STRING, ID_NEW, "New\tCtrl+N");
  AppendMenuA(file, MF_STRING, ID_OPEN, "Open");
  AppendMenuA(file, MF_STRING | MF_GRAYED, ID_SAVE, "Save");
  AppendMenuA(file, MF_SEPARATOR, 0, 0);
  AppendMenuA(file, MF_STRING, ID_EXIT, "E&xit");
  AppendMenuA(edit, MF_STRING, ID_CUT, "Cut");
  AppendMenuA(edit, MF_STRING | MF_CHECKED, ID_COPY, "Copy");
  AppendMenuA(more, MF_STRING, ID_DEEP, "Deep");
  AppendMenuA(edit, MF_STRING | MF_POPUP, (UINT_PTR)more, "More");
  AppendMenuA(g_bar, MF_STRING | MF_POPUP, (UINT_PTR)file, "&File");
  AppendMenuA(g_bar, MF_STRING | MF_POPUP, (UINT_PTR)edit, "&Edit");
  g_popup = CreatePopupMenu();
  AppendMenuA(g_popup, MF_STRING, ID_ALPHA, "Alpha");
  AppendMenuA(g_popup, MF_STRING, ID_BETA, "Beta");
  acc[0].fVirt = FVIRTKEY | FCONTROL;
  acc[0].key = 'N';
  acc[0].cmd = ID_NEW;
  ha = CreateAcceleratorTableA(acc, 1);
  hwnd = CreateWindowExA(0, "WinCloneMenus", "Menus", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 300, 0, g_bar, inst, 0);
  if (hwnd == 0) {
    return 2;
  }
  api_checks(hwnd);
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);
  while (GetMessageA(&msg, 0, 0, 0)) {
    if (!TranslateAcceleratorA(hwnd, ha, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }
  return (int)msg.wParam;
}
