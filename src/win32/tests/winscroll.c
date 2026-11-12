// Scroll bar test: a vertical and a horizontal SCROLLBAR control. The parent moves the thumbs in
// answer to WM_VSCROLL / WM_HSCROLL, as on Windows. The kernel self-test clicks the arrows and the
// track, drags the thumb, clicks a disabled arrow and uses the keyboard; pixels show the thumb
// moving. The exit code is a bit mask of the checks that failed (0 = all passed).
#include <windows.h>

#define ID_V 1
#define ID_H 2

static unsigned g_bits;
static HWND g_v, g_h;
static int g_v_up, g_v_down, g_v_pgup, g_v_pgdn, g_v_track, g_v_thumbpos = -1, g_v_top, g_v_bottom, g_v_end;
static int g_h_left, g_h_right, g_h_pgdn, g_h_end;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

// What an application does with a scroll message: move the thumb.
static void scroll(HWND bar, int code, int pos, int page) {
  SCROLLINFO si;
  int cur = GetScrollPos(bar, SB_CTL), lo, hi, np = cur;

  GetScrollRange(bar, SB_CTL, &lo, &hi);
  si.cbSize = sizeof(si);
  si.fMask = SIF_PAGE;
  GetScrollInfo(bar, SB_CTL, &si);
  hi -= page > 0 ? (int)si.nPage - 1 : 0;
  switch (code) {
    case SB_LINEUP: np = cur - 1; break;
    case SB_LINEDOWN: np = cur + 1; break;
    case SB_PAGEUP: np = cur - page; break;
    case SB_PAGEDOWN: np = cur + page; break;
    case SB_THUMBTRACK: np = pos; break;
    case SB_TOP: np = lo; break;
    case SB_BOTTOM: np = hi; break;
    default: return;
  }
  SetScrollPos(bar, SB_CTL, np, TRUE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      HINSTANCE inst = GetModuleHandleA(0);
      SCROLLINFO si;
      int lo = -1, hi = -1;

      g_v = CreateWindowExA(0, "SCROLLBAR", "", WS_CHILD | WS_VISIBLE | SBS_VERT, 10, 10, 17, 150, h, (HMENU)ID_V, inst, 0);
      g_h = CreateWindowExA(0, "SCROLLBAR", "", WS_CHILD | WS_VISIBLE | SBS_HORZ, 40, 10, 200, 17, h, (HMENU)ID_H, inst, 0);
      CHECK(0, g_v != 0 && g_h != 0);
      si.cbSize = sizeof(si);
      si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
      si.nMin = 0;
      si.nMax = 99;
      si.nPage = 10;
      si.nPos = 0;
      CHECK(1, SetScrollInfo(g_v, SB_CTL, &si, TRUE) == 0);
      SetScrollRange(g_h, SB_CTL, 0, 50, TRUE);
      EnableScrollBar(g_h, SB_CTL, ESB_DISABLE_LTUP);                    // the left arrow is dead
      // The API: ranges, clamping to max - page + 1, and the same through messages.
      ZeroMemory(&si, sizeof(si));
      si.cbSize = sizeof(si);
      si.fMask = SIF_ALL;
      CHECK(2, GetScrollInfo(g_v, SB_CTL, &si) && si.nMin == 0 && si.nMax == 99 && si.nPage == 10 && si.nPos == 0);
      CHECK(3, SetScrollPos(g_v, SB_CTL, 500, FALSE) == 0 && GetScrollPos(g_v, SB_CTL) == 90);      // the largest position is 99 - 10 + 1
      SetScrollPos(g_v, SB_CTL, 0, TRUE);
      CHECK(4, GetScrollRange(g_h, SB_CTL, &lo, &hi) && lo == 0 && hi == 50);
      CHECK(5, SendMessageA(g_h, SBM_SETPOS, 20, FALSE) == 0 && SendMessageA(g_h, SBM_GETPOS, 0, 0) == 20);
      SendMessageA(g_h, SBM_SETPOS, 0, TRUE);
      // The scroll bars of an ordinary window keep their state too.
      CHECK(6, SetScrollRange(h, SB_VERT, 0, 10, FALSE) && SetScrollPos(h, SB_VERT, 5, FALSE) == 0 && GetScrollPos(h, SB_VERT) == 5);
      SetScrollPos(h, SB_VERT, 99, FALSE);
      CHECK(7, GetScrollPos(h, SB_VERT) == 10 && GetScrollPos(h, SB_HORZ) == 0);
      SetFocus(g_v);
      return 0;
    }
    case WM_VSCROLL: {
      int code = LOWORD(wp), pos = (short)HIWORD(wp);

      if ((HWND)lp != g_v) { FAIL(8); return 0; }
      switch (code) {
        case SB_LINEUP: ++g_v_up; break;
        case SB_LINEDOWN: ++g_v_down; break;
        case SB_PAGEUP: ++g_v_pgup; break;
        case SB_PAGEDOWN: ++g_v_pgdn; break;
        case SB_THUMBTRACK: ++g_v_track; break;
        case SB_THUMBPOSITION: g_v_thumbpos = pos; break;
        case SB_TOP: ++g_v_top; break;
        case SB_BOTTOM: ++g_v_bottom; break;
        case SB_ENDSCROLL: ++g_v_end; break;
        default: break;
      }
      scroll(g_v, code, pos, 10);
      return 0;
    }
    case WM_HSCROLL: {
      int code = LOWORD(wp), pos = (short)HIWORD(wp);

      if ((HWND)lp != g_h) { FAIL(9); return 0; }
      switch (code) {
        case SB_LINELEFT: ++g_h_left; break;
        case SB_LINERIGHT: ++g_h_right; break;
        case SB_PAGERIGHT: ++g_h_pgdn; break;
        case SB_ENDSCROLL: ++g_h_end; break;
        default: break;
      }
      scroll(g_h, code, pos, 10);
      return 0;
    }
    case WM_CLOSE:
      CHECK(10, g_v_down == 2 && g_v_up == 2 && g_v_pgdn == 2 && g_v_pgup == 0);              // arrow / page clicks and then the keys Down, Up, PageDown
      CHECK(11, g_v_track >= 1 && g_v_thumbpos == 90);                                          // the thumb dragged to the bottom
      CHECK(12, g_v_top == 1 && g_v_bottom == 1);                                               // Home and End
      CHECK(13, g_v_end == 9);                                                                  // after each click and drag, and each key
      CHECK(14, GetScrollPos(g_v, SB_CTL) == 89);
      CHECK(15, g_h_right == 1 && g_h_pgdn == 1 && g_h_left == 0 && g_h_end == 2);              // the dead left arrow sent nothing
      CHECK(16, GetScrollPos(g_h, SB_CTL) == 11);
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

  (void)prev;
  (void)cmd;
  ZeroMemory(&wc, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hbrBackground = CreateSolidBrush(RGB(200, 220, 255));
  wc.lpszClassName = "WinCloneScroll";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneScroll", "Scroll bars", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 330, 0, 0, inst, 0);
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
