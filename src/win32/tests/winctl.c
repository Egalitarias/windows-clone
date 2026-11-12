// Standard controls test: child windows, BUTTON / STATIC / EDIT / LISTBOX, the focus, Tab and
// Enter through IsDialogMessage, a custom child window. The kernel self-test injects clicks and
// keys; when the window closes the exit code is a bit mask of the checks that failed (0 = all
// passed). The window is 404 x 261 inside.
#include <windows.h>

#define ID_NAME   100
#define ID_EDIT   101
#define ID_OK     102
#define ID_CHECK  103
#define ID_R1     104
#define ID_R2     105
#define ID_LIST   106
#define ID_OFF    107
#define ID_GROUP  108
#define ID_NUM    109

static unsigned g_bits;
static int g_ok_clicks, g_off_clicks, g_check_clicks, g_changes, g_sel_changes, g_dbl, g_panel_x = -1, g_panel_y = -1;
static HWND g_main, g_list, g_edit, g_ok, g_panel;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      HBRUSH red = CreateSolidBrush(RGB(200, 40, 40));
      RECT r = {0, 0, 100, 50};
      FillRect(dc, &r, red);
      DeleteObject(red);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN:
      g_panel_x = (short)LOWORD(lp);
      g_panel_y = (short)HIWORD(lp);
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

// The API checks that need no input.
static void api_checks(HWND main) {
  char t[64];
  HWND list = GetDlgItem(main, ID_LIST), edit = GetDlgItem(main, ID_EDIT);
  int i;
  DWORD a = 0, b = 0;
  BOOL ok = FALSE;

  CHECK(0, list != 0 && edit != 0 && GetDlgItem(main, ID_OK) != 0 && GetDlgItem(main, 9999) == 0);
  CHECK(1, GetParent(edit) == main && IsChild(main, edit) && GetDlgCtrlID(edit) == ID_EDIT);
  CHECK(2, GetDlgItemTextA(main, ID_EDIT, t, sizeof(t)) == 3 && streq(t, "abc") && GetWindowTextLengthA(edit) == 3);
  SendMessageA(edit, EM_SETSEL, 1, 2);
  SendMessageA(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
  CHECK(3, a == 1 && b == 2);
  SendMessageA(edit, EM_SETSEL, 3, 3);
  SetDlgItemInt(main, ID_NUM, 12345, FALSE);
  CHECK(4, GetDlgItemInt(main, ID_NUM, &ok, FALSE) == 12345 && ok);
  SetDlgItemInt(main, ID_NUM, (UINT)-17, TRUE);
  CHECK(5, (int)GetDlgItemInt(main, ID_NUM, &ok, TRUE) == -17 && ok);
  SetDlgItemTextA(main, ID_NUM, "");
  for (i = 0; i < 12; ++i) {
    char s[16] = "item ";
    s[5] = (char)('A' + i);
    s[6] = 0;
    if (SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)s) != i) FAIL(6);
  }
  CHECK(7, SendMessageA(list, LB_GETCOUNT, 0, 0) == 12);
  CHECK(8, SendMessageA(list, LB_FINDSTRING, (WPARAM)-1, (LPARAM)"item F") == 5);
  CHECK(9, SendMessageA(list, LB_GETTEXT, 3, (LPARAM)t) == 6 && streq(t, "item D"));
  SendMessageA(list, LB_SETITEMDATA, 2, 777);
  CHECK(10, SendMessageA(list, LB_GETITEMDATA, 2, 0) == 777);
  CHECK(11, SendMessageA(list, LB_GETCURSEL, 0, 0) == LB_ERR);
  SendMessageA(list, LB_SETCURSEL, 0, 0);
  CHECK(12, SendMessageA(list, LB_GETCURSEL, 0, 0) == 0);
  CHECK(13, IsDlgButtonChecked(main, ID_R1) == BST_CHECKED && IsDlgButtonChecked(main, ID_R2) == BST_UNCHECKED);
  CHECK(14, !IsWindowEnabled(GetDlgItem(main, ID_OFF)) && IsWindowEnabled(GetDlgItem(main, ID_OK)));
  CHECK(15, GetWindow(main, GW_CHILD) != 0 && GetWindow(GetDlgItem(main, ID_NAME), GW_HWNDNEXT) == GetDlgItem(main, ID_EDIT));
  {
    HWND ml = CreateWindowExA(0, "EDIT", "", WS_CHILD | ES_MULTILINE, 0, 0, 100, 60, main, (HMENU)300, GetModuleHandleA(0), 0);
    char t2[32];

    SetWindowTextA(ml, "a\r\nbc\r\n");
    CHECK(29, SendMessageA(ml, EM_GETLINECOUNT, 0, 0) == 3 && SendMessageA(ml, EM_LINELENGTH, 2, 0) == 2);
    CHECK(30, GetWindowTextLengthA(ml) == 7 && GetWindowTextA(ml, t2, sizeof(t2)) == 7 && t2[1] == 13 && t2[2] == 10);
    DestroyWindow(ml);
    CHECK(31, !IsWindow(ml));
  }
  {
    POINT p = {0, 0};
    ClientToScreen(g_panel, &p);
    CHECK(16, p.x == 300 && p.y == 190);
    ScreenToClient(g_panel, &p);
    CHECK(17, p.x == 0 && p.y == 0);
  }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      HINSTANCE inst = GetModuleHandleA(0);
      DWORD v = WS_CHILD | WS_VISIBLE;

      g_main = h;
      CreateWindowExA(0, "STATIC", "&Name:", v | SS_LEFT, 10, 10, 54, 14, h, (HMENU)ID_NAME, inst, 0);
      g_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "abc", v | WS_TABSTOP | ES_AUTOHSCROLL, 70, 8, 150, 20, h, (HMENU)ID_EDIT, inst, 0);
      g_ok = CreateWindowExA(0, "BUTTON", "OK", v | WS_TABSTOP | BS_DEFPUSHBUTTON, 10, 40, 80, 24, h, (HMENU)ID_OK, inst, 0);
      EnableWindow(CreateWindowExA(0, "BUTTON", "Off", v | WS_TABSTOP | BS_PUSHBUTTON, 100, 40, 80, 24, h, (HMENU)ID_OFF, inst, 0), FALSE);
      CreateWindowExA(0, "BUTTON", "Check me", v | WS_TABSTOP | BS_AUTOCHECKBOX, 10, 70, 110, 16, h, (HMENU)ID_CHECK, inst, 0);
      CreateWindowExA(0, "BUTTON", "One", v | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 10, 95, 100, 16, h, (HMENU)ID_R1, inst, 0);
      CreateWindowExA(0, "BUTTON", "Two", v | BS_AUTORADIOBUTTON, 10, 115, 100, 16, h, (HMENU)ID_R2, inst, 0);
      CreateWindowExA(0, "BUTTON", "Group", v | BS_GROUPBOX, 4, 138, 190, 60, h, (HMENU)ID_GROUP, inst, 0);
      CreateWindowExA(0, "EDIT", "", v | WS_TABSTOP | ES_NUMBER, 10, 160, 100, 20, h, (HMENU)ID_NUM, inst, 0);
      g_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "", v | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 200, 40, 150, 100, h, (HMENU)ID_LIST, inst, 0);
      g_panel = CreateWindowExA(0, "PanelClass", "", v, 300, 190, 100, 50, h, (HMENU)200, inst, 0);
      CheckRadioButton(h, ID_R1, ID_R2, ID_R1);
      api_checks(h);
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp), code = HIWORD(wp);

      if (id == ID_OK && code == BN_CLICKED) ++g_ok_clicks;
      if (id == ID_OFF) ++g_off_clicks;
      if (id == ID_CHECK && code == BN_CLICKED) ++g_check_clicks;
      if (id == ID_EDIT && code == EN_CHANGE) ++g_changes;
      if (id == ID_LIST && code == LBN_SELCHANGE) ++g_sel_changes;
      if (id == ID_LIST && code == LBN_DBLCLK) ++g_dbl;
      return 0;
    }
    case WM_CLOSE: {
      char t[64];
      HWND list = GetDlgItem(h, ID_LIST);

      CHECK(18, g_ok_clicks == 3);                                    // mouse, space, Enter
      CHECK(19, g_off_clicks == 0);                                   // a disabled button gets no clicks
      CHECK(20, g_check_clicks == 1 && IsDlgButtonChecked(h, ID_CHECK) == BST_CHECKED);
      CHECK(21, IsDlgButtonChecked(h, ID_R2) == BST_CHECKED && IsDlgButtonChecked(h, ID_R1) == BST_UNCHECKED);
      GetDlgItemTextA(h, ID_EDIT, t, sizeof(t));
      CHECK(22, streq(t, "axybc"));                                   // click after 'a', typed xyz, backspace
      CHECK(23, g_changes == 4);
      CHECK(24, g_sel_changes >= 2 && g_dbl == 1);
      CHECK(25, SendMessageA(list, LB_GETCURSEL, 0, 0) == 4);         // the Down key moved it from item 3
      CHECK(26, g_panel_x == 10 && g_panel_y == 10);                  // child coordinates
      GetDlgItemTextA(h, ID_NUM, t, sizeof(t));
      CHECK(27, streq(t, "42"));                                      // ES_NUMBER took the digits only
      CHECK(28, GetFocus() == list);
      DestroyWindow(h);
      return 0;
    }
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

  (void)prev;
  (void)cmd;
  ZeroMemory(&wc, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hbrBackground = CreateSolidBrush(RGB(200, 220, 255));
  wc.lpszClassName = "WinCloneControls";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  wc.lpfnWndProc = PanelProc;
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = "PanelClass";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneControls", "Controls", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 300, 0, 0, inst, 0);
  if (hwnd == 0) {
    return 2;
  }
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);
  while (GetMessageA(&msg, 0, 0, 0)) {
    if (!IsDialogMessageA(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }
  return (int)msg.wParam;
}
