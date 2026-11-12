// Window scroll bars: WS_VSCROLL / WS_HSCROLL in the frame of a top-level window and of a child
// window. The window handles WM_VSCROLL / WM_HSCROLL itself and moves a marker. The kernel
// self-test clicks the arrows and the track, drags the thumb, clicks the corner and the client
// area, hides the vertical bar with a key and shows it again; pixels show the bars and the marker.
// The exit code is a bit mask of the checks that failed (0 = all passed).
#include <windows.h>

static unsigned g_bits;
static HWND g_main, g_pane;
static int g_v_down, g_v_pgdn, g_v_track, g_v_thumbpos = -1, g_v_end, g_h_right, g_h_end, g_clicks;
static int g_pane_down, g_wm_hidden, g_wm_shown, g_wm_last;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

// What an application does with a scroll message: move the thumb.
static int scroll(HWND h, int bar, int code, int pos, int page) {
  SCROLLINFO si;
  int cur = GetScrollPos(h, bar), np = cur;

  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE;
  GetScrollInfo(h, bar, &si);
  switch (code) {
    case SB_LINEUP: np = cur - 1; break;
    case SB_LINEDOWN: np = cur + 1; break;
    case SB_PAGEUP: np = cur - page; break;
    case SB_PAGEDOWN: np = cur + page; break;
    case SB_THUMBTRACK: np = pos; break;
    case SB_TOP: np = si.nMin; break;
    case SB_BOTTOM: np = si.nMax - (int)si.nPage + 1; break;
    default: return cur;
  }
  SetScrollPos(h, bar, np, TRUE);
  return np;
}

static LRESULT CALLBACK PaneProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      HBRUSH b = CreateSolidBrush(RGB(0, 128, 0));
      RECT r;

      GetClientRect(h, &r);
      FillRect(dc, &r, b);
      DeleteObject(b);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_VSCROLL:
      if (lp != 0) FAIL(11);                                    // a window's own bar: lParam is 0
      if (LOWORD(wp) == SB_LINEDOWN) ++g_pane_down;
      scroll(h, SB_VERT, LOWORD(wp), (short)HIWORD(wp), 3);
      return 0;
    default:
      return DefWindowProcA(h, m, wp, lp);
  }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      RECT r;
      SCROLLINFO si;

      g_main = h;
      GetClientRect(h, &r);
      CHECK(0, r.right == 387 && r.bottom == 274);              // 404 x 291 inside the frame, less two 17-pixel bars
      GetWindowRect(h, &r);
      CHECK(1, r.right - r.left == 420 && r.bottom - r.top == 330);
      si.cbSize = sizeof(si);
      si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
      si.nMin = 0; si.nMax = 99; si.nPage = 10; si.nPos = 0;
      CHECK(2, SetScrollInfo(h, SB_VERT, &si, TRUE) == 0);
      si.nMax = 49; si.nPage = 0;
      SetScrollInfo(h, SB_HORZ, &si, TRUE);
      GetClientRect(h, &r);
      CHECK(3, r.right == 387 && r.bottom == 274);              // the bars stayed
      g_pane = CreateWindowExA(0, "WinClonePane", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 250, 20, 120, 100, h, 0, GetModuleHandleA(0), 0);
      CHECK(4, g_pane != 0);
      si.fMask = SIF_RANGE | SIF_PAGE;
      si.nMin = 0; si.nMax = 9; si.nPage = 3;
      SetScrollInfo(g_pane, SB_VERT, &si, TRUE);
      GetClientRect(g_pane, &r);
      CHECK(5, r.right == 103 && r.bottom == 100);
      return 0;
    }
    case WM_SIZE:
      g_wm_last = LOWORD(lp);
      if (LOWORD(lp) == 404) ++g_wm_hidden;                     // the vertical bar went away
      if (LOWORD(lp) == 387) ++g_wm_shown;
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
      RECT r;

      r.left = 10 + GetScrollPos(h, SB_HORZ) * 4;               // a marker that shows the scroll position
      r.top = 10 + GetScrollPos(h, SB_VERT) * 2;
      r.right = r.left + 20;
      r.bottom = r.top + 20;
      FillRect(dc, &r, red);
      DeleteObject(red);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN:
      ++g_clicks;                                                // only the client area reaches the program as a click
      return 0;
    case WM_CHAR:
      if (wp == 'h') ShowScrollBar(h, SB_VERT, FALSE);
      if (wp == 's') ShowScrollBar(h, SB_VERT, TRUE);
      return 0;
    case WM_VSCROLL: {
      int code = LOWORD(wp);

      if (lp != 0) FAIL(12);
      switch (code) {
        case SB_LINEDOWN: ++g_v_down; break;
        case SB_PAGEDOWN: ++g_v_pgdn; break;
        case SB_THUMBTRACK: ++g_v_track; break;
        case SB_THUMBPOSITION: g_v_thumbpos = (short)HIWORD(wp); break;
        case SB_ENDSCROLL: ++g_v_end; break;
        default: break;
      }
      scroll(h, SB_VERT, code, (short)HIWORD(wp), 10);
      InvalidateRect(h, 0, TRUE);
      return 0;
    }
    case WM_HSCROLL: {
      int code = LOWORD(wp);

      switch (code) {
        case SB_LINERIGHT: ++g_h_right; break;
        case SB_ENDSCROLL: ++g_h_end; break;
        default: break;
      }
      scroll(h, SB_HORZ, code, (short)HIWORD(wp), 7);
      InvalidateRect(h, 0, TRUE);
      return 0;
    }
    case WM_CLOSE: {
      RECT r;

      CHECK(6, g_v_down == 1 && g_v_pgdn == 1 && g_v_track >= 1 && g_v_thumbpos == 90);
      CHECK(7, g_v_end == 3 && g_h_right == 1 && g_h_end == 1);  // the corner and the dead area sent nothing
      CHECK(8, g_clicks == 1);
      CHECK(9, g_pane_down == 1 && GetScrollPos(g_pane, SB_VERT) == 1);
      CHECK(10, g_wm_hidden == 1 && g_wm_shown >= 1);
      GetClientRect(h, &r);
      CHECK(13, r.right == 387 && r.bottom == 274 && g_wm_last == 387);
      CHECK(14, GetScrollPos(h, SB_VERT) == 90 && GetScrollPos(h, SB_HORZ) == 1);
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
  wc.lpfnWndProc = PaneProc;
  wc.hInstance = inst;
  wc.lpszClassName = "WinClonePane";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  ZeroMemory(&wc, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hbrBackground = CreateSolidBrush(RGB(200, 220, 255));
  wc.lpszClassName = "WinCloneBars";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneBars", "Frame scroll bars", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
                         CW_USEDEFAULT, CW_USEDEFAULT, 420, 330, 0, 0, inst, 0);
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
