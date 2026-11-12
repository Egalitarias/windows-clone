// Combo box test: a CBS_DROPDOWNLIST and an editable CBS_DROPDOWN. The kernel self-test clicks
// the fields and buttons, chooses items with the mouse, types, uses Down/Enter/Escape, and
// checks pixels (the list appears over the parent and is painted away again). The exit code is a
// bit mask of the checks that failed (0 = all passed).
#include <windows.h>

#define ID_A 1
#define ID_B 2

static unsigned g_bits;
static int g_sel, g_edit, g_drop, g_close, g_ok, g_cancel;
static HWND g_a, g_b;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      HINSTANCE inst = GetModuleHandleA(0);
      static const char *colors[] = {"Red", "Green", "Blue", "Yellow", "Cyan"};
      static const char *greek[] = {"alpha", "beta", "gamma"};
      int i;
      RECT r;
      char t[16];

      g_a = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 10, 10, 150, 140, h, (HMENU)ID_A, inst, 0);
      g_b = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN, 200, 10, 150, 140, h, (HMENU)ID_B, inst, 0);
      CHECK(0, g_a != 0 && g_b != 0);
      for (i = 0; i < 5; ++i) CHECK(1, SendMessageA(g_a, CB_ADDSTRING, 0, (LPARAM)colors[i]) == i);
      for (i = 0; i < 3; ++i) SendMessageA(g_b, CB_ADDSTRING, 0, (LPARAM)greek[i]);
      CHECK(2, SendMessageA(g_a, CB_GETCOUNT, 0, 0) == 5 && SendMessageA(g_b, CB_GETCOUNT, 0, 0) == 3);
      CHECK(3, SendMessageA(g_a, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)"Green") == 1);
      CHECK(4, SendMessageA(g_a, CB_GETCURSEL, 0, 0) == CB_ERR);
      GetClientRect(g_a, &r);
      CHECK(5, r.bottom == 20 && r.right == 150);                    // closed: the field's height only
      CHECK(6, SendMessageA(g_a, CB_GETLBTEXT, 3, (LPARAM)t) == 6 && streq(t, "Yellow"));
      CHECK(7, SendMessageA(g_a, CB_GETDROPPEDSTATE, 0, 0) == 0);
      return 0;
    }
    case WM_COMMAND:
      switch (HIWORD(wp)) {
        case CBN_SELCHANGE: ++g_sel; break;
        case CBN_EDITCHANGE: ++g_edit; break;
        case CBN_DROPDOWN: ++g_drop; break;
        case CBN_CLOSEUP: ++g_close; break;
        case CBN_SELENDOK: ++g_ok; break;
        case CBN_SELENDCANCEL: ++g_cancel; break;
        default: break;
      }
      return 0;
    case WM_CLOSE: {
      char t[32];

      CHECK(8, SendMessageA(g_a, CB_GETCURSEL, 0, 0) == 4);           // Cyan, by type-ahead
      GetWindowTextA(g_a, t, sizeof(t));
      CHECK(9, streq(t, "Cyan"));
      GetWindowTextA(g_b, t, sizeof(t));
      CHECK(10, streq(t, "beta") && SendMessageA(g_b, CB_GETCURSEL, 0, 0) == 1);
      CHECK(11, g_sel == 6);                                          // A: click, Down while dropped, Down closed, 'c'; B: Down, Down
      CHECK(12, g_edit == 2);                                         // the two characters typed in B (choosing an item is not an edit)
      CHECK(13, g_drop == 3 && g_close == 3);
      CHECK(14, g_ok == 2 && g_cancel == 1);
      CHECK(15, SendMessageA(g_a, CB_GETDROPPEDSTATE, 0, 0) == 0 && SendMessageA(g_b, CB_GETDROPPEDSTATE, 0, 0) == 0);
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
  wc.lpszClassName = "WinCloneCombos";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneCombos", "Combos", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 330, 0, 0, inst, 0);
  if (hwnd == 0) {
    return 2;
  }
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);
  while (GetMessageA(&msg, 0, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  return (int)msg.wParam;
}
