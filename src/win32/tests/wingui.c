// GUI test: a window class, a message loop, WM_PAINT with GDI, mouse, keyboard and a timer.
// The exit code is 10 * mouse clicks + typed characters + 100 if the timer fired, so the
// kernel self-test can check what the program saw.
#include <windows.h>

static int g_clicks, g_chars, g_ticks;
static int g_cx = -100, g_cy = -100;
static char g_text[64];

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      SetTimer(hwnd, 1, 40, 0);
      return 0;
    case WM_TIMER:
      ++g_ticks;
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      HBRUSH red = CreateSolidBrush(RGB(200, 40, 40));
      HBRUSH green = CreateSolidBrush(RGB(40, 200, 40));
      HBRUSH blue = CreateSolidBrush(RGB(40, 40, 200));
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
      RECT r = {10, 10, 60, 60}, m;
      HGDIOBJ old;

      old = SelectObject(dc, pen);
      FillRect(dc, &r, red);
      r.left = 100; r.top = 10; r.right = 160; r.bottom = 60;
      SelectObject(dc, blue);
      Rectangle(dc, r.left, r.top, r.right, r.bottom);
      SelectObject(dc, green);
      Ellipse(dc, 200, 10, 260, 60);
      MoveToEx(dc, 10, 80, 0);
      LineTo(dc, 250, 120);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(0, 0, 255));
      TextOutA(dc, 10, 150, "WinClone Win32", 14);
      SetTextColor(dc, RGB(255, 0, 255));
      TextOutA(dc, 10, 200, g_text, lstrlenA(g_text));
      if (g_cx >= 0) {
        m.left = g_cx - 5; m.top = g_cy - 5; m.right = g_cx + 5; m.bottom = g_cy + 5;
        FillRect(dc, &m, green);
      }
      SelectObject(dc, old);
      DeleteObject(red);
      DeleteObject(green);
      DeleteObject(blue);
      DeleteObject(pen);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN:
      ++g_clicks;
      g_cx = (short)LOWORD(lp);
      g_cy = (short)HIWORD(lp);
      InvalidateRect(hwnd, 0, FALSE);
      return 0;
    case WM_CHAR:
      if (wp >= 32 && g_chars < 60) {
        g_text[g_chars++] = (char)wp;
        g_text[g_chars] = 0;
        InvalidateRect(hwnd, 0, FALSE);
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(g_clicks * 10 + g_chars + (g_ticks > 0 ? 100 : 0));
      return 0;
    default:
      return DefWindowProcA(hwnd, msg, wp, lp);
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
  wc.hCursor = LoadCursorA(0, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = "WinCloneTest";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneTest", "Win32 on WinClone", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                         CW_USEDEFAULT, 416, 339, 0, 0, inst, 0);
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
