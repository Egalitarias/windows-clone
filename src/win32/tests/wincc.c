// Common controls: progress bar, trackbar, up-down with an edit as its buddy, tab control and status
// bar. The kernel self-test clicks and drags them; pixels show the result, and the program checks the
// messages and notifications it received. The exit code is a bit mask of failed checks (0 = passed).
#include <windows.h>
#include <commctrl.h>

static unsigned g_bits;
static HWND g_progress, g_track, g_edit, g_ud, g_tab, g_status;
static int g_tb_track, g_tb_pos, g_tb_page, g_tb_end, g_ud_msgs, g_ud_end, g_ud_delta, g_tab_changed, g_tab_changing;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      HINSTANCE inst = GetModuleHandleA(0);
      TCITEMA it;
      int parts[3] = {150, 300, -1};
      RECT r;
      static const char *names[3] = {"One", "Two", "Three"};
      int i;
      LRESULT v;

      InitCommonControls();
      g_progress = CreateWindowExA(0, PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 10, 10, 200, 20, h, (HMENU)101, inst, 0);
      SendMessageA(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
      SendMessageA(g_progress, PBM_SETSTEP, 20, 0);
      SendMessageA(g_progress, PBM_SETPOS, 30, 0);
      CHECK(1, SendMessageA(g_progress, PBM_STEPIT, 0, 0) == 30 && SendMessageA(g_progress, PBM_GETPOS, 0, 0) == 50);
      CHECK(2, SendMessageA(g_progress, PBM_DELTAPOS, 100, 0) == 50 && SendMessageA(g_progress, PBM_GETPOS, 0, 0) == 100);   // clamped to the range
      SendMessageA(g_progress, PBM_SETPOS, 50, 0);

      g_track = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 10, 50, 200, 30, h, (HMENU)102, inst, 0);
      SendMessageA(g_track, TBM_SETRANGE, TRUE, MAKELONG(0, 10));
      SendMessageA(g_track, TBM_SETPAGESIZE, 0, 3);
      CHECK(3, SendMessageA(g_track, TBM_GETRANGEMAX, 0, 0) == 10 && SendMessageA(g_track, TBM_GETPAGESIZE, 0, 0) == 3 && SendMessageA(g_track, TBM_GETPOS, 0, 0) == 0);

      g_edit = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 10, 100, 60, 22, h, (HMENU)103, inst, 0);
      g_ud = CreateWindowExA(0, UPDOWN_CLASSA, "", WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT, 0, 0, 0, 0, h, (HMENU)104, inst, 0);
      SendMessageA(g_ud, UDM_SETBUDDY, (WPARAM)g_edit, 0);
      SendMessageA(g_ud, UDM_SETRANGE, 0, MAKELONG(10, 0));
      CHECK(4, SendMessageA(g_ud, UDM_SETPOS, 0, 5) == 0 && SendMessageA(g_ud, UDM_SETPOS, 0, 100) == 5 && SendMessageA(g_ud, UDM_GETPOS, 0, 0) == 10);   // clamped to 10
      SendMessageA(g_ud, UDM_SETPOS, 0, 5);
      GetWindowRect(g_ud, &r);
      CHECK(5, r.right - r.left == 17 && r.bottom - r.top == 22 && (HWND)SendMessageA(g_ud, UDM_GETBUDDY, 0, 0) == g_edit);

      g_tab = CreateWindowExA(0, WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE, 10, 140, 240, 90, h, (HMENU)105, inst, 0);
      for (i = 0; i < 3; ++i) {
        ZeroMemory(&it, sizeof(it));
        it.mask = TCIF_TEXT | TCIF_PARAM;
        it.pszText = (LPSTR)names[i];
        it.lParam = 100 + i;
        CHECK(6, SendMessageA(g_tab, TCM_INSERTITEMA, i, (LPARAM)&it) == i);
      }
      CHECK(7, SendMessageA(g_tab, TCM_GETITEMCOUNT, 0, 0) == 3 && SendMessageA(g_tab, TCM_GETCURSEL, 0, 0) == 0);
      SetRect(&r, 0, 0, 200, 100);
      SendMessageA(g_tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&r);
      v = r.top > 0 && r.left > 0 && r.right < 200;
      SendMessageA(g_tab, TCM_ADJUSTRECT, TRUE, (LPARAM)&r);
      CHECK(8, v && r.left == 0 && r.top == 0 && r.right == 200 && r.bottom == 100);
      {
        char buf[16];

        ZeroMemory(&it, sizeof(it));
        it.mask = TCIF_TEXT | TCIF_PARAM;
        it.pszText = buf;
        it.cchTextMax = sizeof(buf);
        CHECK(9, SendMessageA(g_tab, TCM_GETITEMA, 2, (LPARAM)&it) && buf[0] == 'T' && buf[4] == 'e' && it.lParam == 102);
      }

      g_status = CreateWindowExA(0, STATUSCLASSNAMEA, "Ready", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, h, (HMENU)106, inst, 0);
      SendMessageA(g_status, SB_SETPARTS, 3, (LPARAM)parts);
      SendMessageA(g_status, SB_SETTEXTA, 1, (LPARAM)"Part 2");
      SendMessageA(g_status, SB_SETTEXTA, 2, (LPARAM)"Three");
      return 0;
    }
    case WM_SIZE:
      if (g_status != 0) SendMessageA(g_status, WM_SIZE, 0, 0);
      return 0;
    case WM_HSCROLL:
      if ((HWND)lp == g_track) {
        switch (LOWORD(wp)) {
          case TB_THUMBTRACK: ++g_tb_track; break;
          case TB_THUMBPOSITION: ++g_tb_pos; break;
          case TB_PAGEDOWN: ++g_tb_page; break;
          case TB_ENDTRACK: ++g_tb_end; break;
          default: break;
        }
      }
      return 0;
    case WM_VSCROLL:
      if ((HWND)lp == g_ud) {
        if (LOWORD(wp) == SB_THUMBPOSITION) ++g_ud_msgs;
        if (LOWORD(wp) == SB_ENDSCROLL) ++g_ud_end;
      }
      return 0;
    case WM_NOTIFY: {
      const NMHDR *nm = (const NMHDR *)lp;

      if (nm->hwndFrom == g_ud && nm->code == UDN_DELTAPOS) ++g_ud_delta;
      if (nm->hwndFrom == g_tab && nm->code == TCN_SELCHANGING) ++g_tab_changing;
      if (nm->hwndFrom == g_tab && nm->code == TCN_SELCHANGE) {
        ++g_tab_changed;
        InvalidateRect(h, 0, TRUE);
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      static const COLORREF colours[3] = {RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255)};
      int sel = (int)SendMessageA(g_tab, TCM_GETCURSEL, 0, 0);
      HBRUSH b = CreateSolidBrush(colours[sel < 0 || sel > 2 ? 0 : sel]);
      RECT r = {20, 240, 240, 260};

      FillRect(dc, &r, b);                                       // what the selected tab shows
      DeleteObject(b);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CLOSE: {
      int parts[3] = {0, 0, 0}, i;
      char buf[16] = "", t0[16] = "", t1[16] = "";
      RECT cr, sr;
      POINT p;
      LRESULT ud = SendMessageA(g_ud, UDM_GETPOS, 0, 0);

      CHECK(10, SendMessageA(g_progress, PBM_GETPOS, 0, 0) == 50 && SendMessageA(g_progress, PBM_GETRANGE, FALSE, 0) == 100);
      CHECK(11, SendMessageA(g_track, TBM_GETPOS, 0, 0) == 5 && g_tb_page == 1 && g_tb_track >= 1 && g_tb_pos == 1 && g_tb_end == 2);
      GetWindowTextA(g_edit, buf, sizeof(buf));
      CHECK(12, LOWORD(ud) == 6 && HIWORD(ud) == 0 && buf[0] == '6' && buf[1] == 0);                // the buddy shows the position
      CHECK(13, g_ud_msgs == 3 && g_ud_delta == 3 && g_ud_end == 3);
      CHECK(14, SendMessageA(g_tab, TCM_GETCURSEL, 0, 0) == 1 && g_tab_changed == 1 && g_tab_changing == 1);
      CHECK(15, SendMessageA(g_status, SB_GETPARTS, 3, (LPARAM)parts) == 3 && parts[0] == 150 && parts[1] == 300 && parts[2] == -1);
      SendMessageA(g_status, SB_GETTEXTA, 0, (LPARAM)t0);
      SendMessageA(g_status, SB_GETTEXTA, 1, (LPARAM)t1);
      CHECK(16, t0[0] == 'R' && t1[0] == 'P' && t1[5] == '2' && (SendMessageA(g_status, SB_GETTEXTLENGTHA, 2, 0) & 0xFFFF) == 5);
      GetClientRect(h, &cr);
      GetWindowRect(g_status, &sr);
      p.x = sr.left;
      p.y = sr.top;
      ScreenToClient(h, &p);
      CHECK(17, sr.right - sr.left == cr.right && sr.bottom - sr.top == 22 && p.x == 0 && p.y == cr.bottom - 22);          // against the bottom, as wide as the window
      SendMessageA(g_status, SB_SIMPLE, TRUE, 0);
      CHECK(18, SendMessageA(g_status, SB_ISSIMPLE, 0, 0) == TRUE);
      (void)i;
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
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = "WinCloneCommon";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneCommon", "Common controls", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 380, 0, 0, inst, 0);
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
