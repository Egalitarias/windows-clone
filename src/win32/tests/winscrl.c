// ScrollWindow test: the window shows colour bands (one per line, 20 pixels each) and, drawn only
// when the whole client area is painted, a black square. Keys scroll the window: 'd' ScrollWindow
// up one line, 'r' left 30 pixels, 'e' ScrollWindowEx up one line. The program paints only what
// the scroll uncovers (the invalid rectangle), so the square moves only if the pixels were really
// moved, and the kernel self-test checks them. ScrollDC on a memory bitmap and the scroll
// arithmetic are checked in the program. The exit code is a bit mask of the failures (0 = passed).
#include <windows.h>

static unsigned g_bits;
static int g_top;                       // the line at the top of the window
static int g_painted_full, g_painted_part;
static RECT g_upd;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

static COLORREF band(int line) { return RGB((30 + line * 10) & 255, 250 - line * 10, 60 + line * 8); }

static void mem_checks(void) {
  HDC dc = CreateCompatibleDC(0);
  HBITMAP bm = CreateCompatibleBitmap(dc, 8, 8), old;
  RECT r, upd;
  int y;

  old = (HBITMAP)SelectObject(dc, bm);
  for (y = 0; y < 8; ++y) {                                   // row y has the colour (y*10, 0, 0)
    HBRUSH b = CreateSolidBrush(RGB(y * 10, 0, 0));

    SetRect(&r, 0, y, 8, y + 1);
    FillRect(dc, &r, b);
    DeleteObject(b);
  }
  CHECK(0, ScrollDC(dc, 0, -2, 0, 0, 0, &upd));               // up by two
  CHECK(1, GetPixel(dc, 3, 0) == RGB(20, 0, 0) && GetPixel(dc, 3, 5) == RGB(70, 0, 0));
  CHECK(2, upd.left == 0 && upd.right == 8 && upd.top == 6 && upd.bottom == 8);                // the bottom two rows are uncovered
  CHECK(3, ScrollDC(dc, 3, 0, 0, 0, 0, &upd) && GetPixel(dc, 4, 0) == RGB(20, 0, 0) && upd.left == 0 && upd.right == 3 && upd.top == 0 && upd.bottom == 8);
  SetRect(&r, 0, 0, 8, 4);                                    // only the top half
  CHECK(4, ScrollDC(dc, 0, 1, &r, 0, 0, &upd) && GetPixel(dc, 5, 1) == RGB(20, 0, 0) && GetPixel(dc, 5, 6) == RGB(60, 0, 0) && upd.top == 0 && upd.bottom == 1);
  SelectObject(dc, old);
  DeleteObject(bm);
  DeleteDC(dc);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT cl;
      int k;

      GetClientRect(h, &cl);
      for (k = 0; k * 20 < cl.bottom; ++k) {
        RECT row = {0, k * 20, cl.right, k * 20 + 20}, part;
        HBRUSH b = CreateSolidBrush(band(g_top + k));

        if (IntersectRect(&part, &row, &ps.rcPaint)) {
          FillRect(dc, &part, b);
        }
        DeleteObject(b);
      }
      if (ps.rcPaint.left <= 0 && ps.rcPaint.top <= 0 && ps.rcPaint.right >= cl.right && ps.rcPaint.bottom >= cl.bottom) {
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        RECT sq = {200, 45, 220, 65};

        FillRect(dc, &sq, black);                                 // only the first, complete paint draws it
        DeleteObject(black);
        ++g_painted_full;
      } else {
        ++g_painted_part;
      }
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CHAR:
      if (wp == 'd') {
        ++g_top;
        CHECK(5, ScrollWindow(h, 0, -20, 0, 0));
      } else if (wp == 'r') {
        CHECK(6, ScrollWindow(h, -30, 0, 0, 0));
      } else if (wp == 'e') {
        ++g_top;
        CHECK(7, ScrollWindowEx(h, 0, -20, 0, 0, 0, &g_upd, SW_INVALIDATE | SW_ERASE) == SIMPLEREGION);
      }
      return 0;
    case WM_CLOSE:
      CHECK(8, g_painted_full >= 1 && g_painted_part >= 3);                   // the scrolls repainted strips, not the window
      CHECK(9, g_upd.left == 0 && g_upd.right == 404 && g_upd.top == 271 && g_upd.bottom == 291);       // what ScrollWindowEx reported uncovered
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
  mem_checks();
  ZeroMemory(&wc, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hbrBackground = CreateSolidBrush(RGB(255, 255, 255));
  wc.lpszClassName = "WinCloneScrollWin";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneScrollWin", "ScrollWindow", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 330, 0, 0, inst, 0);
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
