// TransparentBlt and AlphaBlend (msimg32) onto a window and between memory DCs, and the window as a
// blit source. The kernel self-test checks the pixels of the surface; the program checks what it can
// read back itself. The exit code is a bit mask of the failures (0 = passed).
#include <windows.h>

static unsigned g_bits;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
  HBRUSH br = CreateSolidBrush(c);
  RECT rc;

  SetRect(&rc, l, t, r, b);
  FillRect(dc, &rc, br);
  DeleteObject(br);
}

// A bitmap of w x h in a memory DC, the left `split` columns in a, the rest in b.
static HBITMAP two_tone(HDC mem, int w, int h, int split, COLORREF a, COLORREF b) {
  HBITMAP bm = CreateCompatibleBitmap(mem, w, h), old = (HBITMAP)SelectObject(mem, bm);

  fill(mem, 0, 0, split, h, a);
  fill(mem, split, 0, w, h, b);
  SelectObject(mem, old);
  return bm;
}

// Everything that does not need the window.
static void mem_checks(void) {
  HDC dc = CreateCompatibleDC(0), sd = CreateCompatibleDC(0);
  HBITMAP db = CreateCompatibleBitmap(dc, 4, 4), dold, sold;
  HBITMAP sb = two_tone(sd, 4, 4, 4, RGB(200, 0, 0), RGB(200, 0, 0));
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, 0};
  HBITMAP two = two_tone(sd, 4, 4, 2, RGB(255, 0, 0), RGB(255, 0, 255));

  dold = (HBITMAP)SelectObject(dc, db);
  sold = (HBITMAP)SelectObject(sd, sb);
  fill(dc, 0, 0, 4, 4, RGB(100, 100, 100));
  CHECK(0, AlphaBlend(dc, 0, 0, 4, 4, sd, 0, 0, 4, 4, bf) && GetPixel(dc, 1, 1) == RGB(200, 0, 0));           // opaque: the source
  fill(dc, 0, 0, 4, 4, RGB(100, 100, 100));
  bf.SourceConstantAlpha = 0;
  CHECK(1, AlphaBlend(dc, 0, 0, 4, 4, sd, 0, 0, 4, 4, bf) && GetPixel(dc, 1, 1) == RGB(100, 100, 100));       // transparent: the destination
  bf.SourceConstantAlpha = 51;                                                                                // a fifth of the source over four fifths of the destination
  CHECK(2, AlphaBlend(dc, 0, 0, 4, 4, sd, 0, 0, 4, 4, bf) && GetPixel(dc, 1, 1) == RGB(120, 80, 80));
  bf.BlendOp = AC_SRC_OVER + 1;
  CHECK(3, !AlphaBlend(dc, 0, 0, 4, 4, sd, 0, 0, 4, 4, bf));                                                  // only AC_SRC_OVER exists
  bf.BlendOp = AC_SRC_OVER;
  bf.AlphaFormat = 2;
  CHECK(4, !AlphaBlend(dc, 0, 0, 4, 4, sd, 0, 0, 4, 4, bf));

  SelectObject(sd, two);                                                                                      // left half red, right half magenta
  fill(dc, 0, 0, 4, 4, RGB(0, 0, 255));
  CHECK(5, TransparentBlt(dc, 0, 0, 4, 4, sd, 0, 0, 4, 4, RGB(255, 0, 255)));
  CHECK(6, GetPixel(dc, 0, 0) == RGB(255, 0, 0) && GetPixel(dc, 3, 3) == RGB(0, 0, 255));                     // the magenta half left the destination alone
  fill(dc, 0, 0, 4, 4, RGB(0, 0, 255));
  CHECK(7, TransparentBlt(dc, 0, 0, 2, 4, sd, 0, 0, 4, 4, RGB(255, 0, 255)) && GetPixel(dc, 0, 0) == RGB(255, 0, 0) && GetPixel(dc, 1, 3) == RGB(0, 0, 255));       // shrunk to half: the columns sampled are red and magenta
  SelectObject(sd, sold);
  SelectObject(dc, dold);
  DeleteObject(db);
  DeleteObject(sb);
  DeleteObject(two);
  DeleteDC(dc);
  DeleteDC(sd);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      HDC sd = CreateCompatibleDC(dc), td;
      HBITMAP key = two_tone(sd, 20, 20, 10, RGB(255, 0, 0), RGB(255, 0, 255)), blue = two_tone(sd, 10, 10, 10, RGB(0, 0, 255), RGB(0, 0, 255)), old;
      BLENDFUNCTION half = {AC_SRC_OVER, 0, 128, 0}, per = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
      BITMAPINFO bi;
      DWORD *bits = 0;
      HBITMAP sect, tb, told;
      int i;

      fill(dc, 0, 0, 400, 300, RGB(200, 220, 255));
      old = (HBITMAP)SelectObject(sd, key);
      CHECK(8, TransparentBlt(dc, 10, 10, 20, 20, sd, 0, 0, 20, 20, RGB(255, 0, 255)));                       // red | (nothing)
      CHECK(9, TransparentBlt(dc, 100, 10, 40, 40, sd, 0, 0, 20, 20, RGB(255, 0, 255)));                      // the same, stretched
      SelectObject(sd, blue);
      fill(dc, 10, 60, 50, 100, RGB(200, 0, 0));
      CHECK(10, AlphaBlend(dc, 10, 60, 40, 40, sd, 0, 0, 10, 10, half));                                      // half blue over red, stretched
      fill(dc, 100, 60, 140, 100, RGB(255, 255, 255));
      ZeroMemory(&bi, sizeof(bi));
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = 4;
      bi.bmiHeader.biHeight = 4;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      sect = CreateDIBSection(sd, &bi, DIB_RGB_COLORS, (void **)&bits, 0, 0);
      CHECK(11, sect != 0 && bits != 0);
      if (bits != 0) {
        for (i = 0; i < 16; ++i) bits[i] = 0x80000080u;                                                       // blue, premultiplied by an alpha of 128
        SelectObject(sd, sect);
        CHECK(12, AlphaBlend(dc, 100, 60, 40, 40, sd, 0, 0, 4, 4, per));
      }
      SelectObject(sd, old);
      // The window as the source of a blit (and what the program can read back from it).
      CHECK(13, BitBlt(dc, 200, 10, 20, 20, dc, 10, 10, SRCCOPY));
      CHECK(14, GetPixel(dc, 15, 15) == RGB(255, 0, 0) && GetPixel(dc, 205, 15) == RGB(255, 0, 0) && GetPixel(dc, 215, 15) == RGB(200, 220, 255));
      CHECK(15, GetPixel(dc, 20, 70) == RGB(100, 0, 128) && GetPixel(dc, 110, 70) == RGB(127, 127, 255));
      CHECK(16, GetPixel(dc, -50, -50) == CLR_INVALID);
      td = CreateCompatibleDC(dc);
      tb = CreateCompatibleBitmap(dc, 4, 4);
      told = (HBITMAP)SelectObject(td, tb);
      CHECK(17, BitBlt(td, 0, 0, 4, 4, dc, 12, 12, SRCCOPY) && GetPixel(td, 1, 1) == RGB(255, 0, 0));          // a window to a bitmap
      // SRCINVERT reads the destination: a window's old pixels are read back.
      CHECK(18, BitBlt(dc, 300, 10, 20, 20, dc, 10, 10, SRCINVERT) && GetPixel(dc, 305, 15) == RGB(55, 220, 255));
      SelectObject(td, told);
      DeleteObject(tb);
      DeleteDC(td);
      DeleteObject(key);
      DeleteObject(blue);
      if (sect != 0) DeleteObject(sect);
      DeleteDC(sd);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CLOSE:
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
  wc.hbrBackground = CreateSolidBrush(RGB(200, 220, 255));
  wc.lpszClassName = "WinCloneBlend";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneBlend", "AlphaBlend", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 330, 0, 0, inst, 0);
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
