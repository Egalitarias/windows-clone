// Bitmap test: memory DCs, bitmaps, BitBlt / StretchBlt with several raster operations, DIBs and a
// bitmap resource. Most checks read pixels back from memory DCs; the window shows blits onto a
// window DC, which the kernel self-test checks by pixel. The exit code is a bit mask of the checks
// that failed (0 = all passed).
#include <windows.h>

static unsigned g_bits;
static HDC g_mem;                     // the canvas: 40x30, left half red, right half blue
static HBITMAP g_canvas, g_pic;       // g_pic: the 4x2 bitmap resource

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

static void make_canvas(void) {
  HDC dc = CreateCompatibleDC(0);
  HBRUSH red = CreateSolidBrush(RGB(255, 0, 0)), blue = CreateSolidBrush(RGB(0, 0, 255));
  RECT l = {0, 0, 20, 30}, r = {20, 0, 40, 30};

  g_canvas = CreateCompatibleBitmap(dc, 40, 30);
  SelectObject(dc, g_canvas);
  FillRect(dc, &l, red);
  FillRect(dc, &r, blue);
  DeleteObject(red);
  DeleteObject(blue);
  g_mem = dc;
}

static void api_checks(HINSTANCE inst) {
  HDC a, b;
  HBITMAP ba, bb, old;
  HBRUSH br;
  BITMAP bm;
  RECT r;
  int x, y, found;
  unsigned char bits24[8] = {0, 0, 255, 255, 0, 0, 0, 0};          // 2x1, 24 bit: red, then blue (BGR); padded to 8 bytes
  BITMAPINFO bi;
  DWORD *sect = 0;
  HBITMAP dibs;
  DWORD dibbuf[2];

  a = CreateCompatibleDC(0);
  b = CreateCompatibleDC(0);
  ba = CreateCompatibleBitmap(a, 8, 8);
  bb = CreateCompatibleBitmap(a, 8, 8);
  CHECK(0, a != 0 && b != 0 && ba != 0 && bb != 0 && GetObjectType(ba) == OBJ_BITMAP);
  old = (HBITMAP)SelectObject(a, ba);
  SelectObject(b, bb);
  CHECK(1, old != 0 && GetObjectA(ba, sizeof(bm), &bm) == sizeof(bm) && bm.bmWidth == 8 && bm.bmHeight == 8 && bm.bmBitsPixel == 32);

  // Fill, GetPixel, and the edges of the bitmap.
  br = CreateSolidBrush(RGB(255, 0, 0));
  SetRect(&r, 0, 0, 8, 8);
  FillRect(a, &r, br);
  CHECK(2, GetPixel(a, 3, 3) == RGB(255, 0, 0) && GetPixel(a, 8, 3) == CLR_INVALID && GetPixel(a, -1, 0) == CLR_INVALID);

  // BitBlt with SRCCOPY, SRCINVERT, NOTSRCCOPY, SRCPAINT, SRCAND, DSTINVERT, PATINVERT.
  CHECK(3, BitBlt(b, 0, 0, 8, 8, a, 0, 0, SRCCOPY) && GetPixel(b, 7, 7) == RGB(255, 0, 0));
  CHECK(4, BitBlt(b, 0, 0, 8, 8, a, 0, 0, SRCINVERT) && GetPixel(b, 2, 2) == RGB(0, 0, 0));
  CHECK(5, BitBlt(b, 0, 0, 8, 8, a, 0, 0, NOTSRCCOPY) && GetPixel(b, 2, 2) == RGB(0, 255, 255));
  BitBlt(b, 0, 0, 8, 8, a, 0, 0, SRCCOPY);
  {
    HBRUSH green = CreateSolidBrush(RGB(0, 255, 0));

    FillRect(b, &r, green);
    CHECK(6, BitBlt(b, 0, 0, 8, 8, a, 0, 0, SRCPAINT) && GetPixel(b, 1, 1) == RGB(255, 255, 0));    // green | red
    CHECK(7, BitBlt(b, 0, 0, 8, 8, a, 0, 0, SRCAND) && GetPixel(b, 1, 1) == RGB(255, 0, 0));   // (red|green) & red
    CHECK(8, PatBlt(b, 0, 0, 8, 8, DSTINVERT) && GetPixel(b, 1, 1) == RGB(0, 255, 255));
    SelectObject(b, green);
    CHECK(9, PatBlt(b, 0, 0, 4, 8, PATINVERT) && GetPixel(b, 1, 1) == RGB(0, 0, 255) && GetPixel(b, 6, 1) == RGB(0, 255, 255));      // cyan ^ green on the left half
    CHECK(10, PatBlt(b, 0, 0, 8, 8, BLACKNESS) && GetPixel(b, 5, 5) == 0 && PatBlt(b, 0, 0, 8, 8, WHITENESS) && GetPixel(b, 5, 5) == RGB(255, 255, 255));
    SelectObject(b, GetStockObject(WHITE_BRUSH));
    DeleteObject(green);
  }

  // StretchBlt: 2x, and flipped by a negative width. The canvas has red on the left, blue on the right.
  CHECK(11, StretchBlt(b, 0, 0, 8, 8, g_mem, 0, 0, 40, 30, SRCCOPY) && GetPixel(b, 1, 4) == RGB(255, 0, 0) && GetPixel(b, 6, 4) == RGB(0, 0, 255));
  CHECK(12, StretchBlt(b, 8, 0, -8, 8, g_mem, 0, 0, 40, 30, SRCCOPY) && GetPixel(b, 1, 4) == RGB(0, 0, 255) && GetPixel(b, 6, 4) == RGB(255, 0, 0));
  // Clipping at the destination's edges, and a source rectangle that is partly outside: no crash, the rest is drawn.
  CHECK(13, BitBlt(b, 4, 4, 40, 30, g_mem, 0, 0, SRCCOPY) && GetPixel(b, 5, 5) == RGB(255, 0, 0) && BitBlt(b, -10, -10, 20, 20, g_mem, 0, 0, SRCCOPY));

  // A bitmap cannot be deleted while it is selected into a DC.
  CHECK(14, !DeleteObject(ba));
  SelectObject(a, old);
  CHECK(15, DeleteObject(ba) && GetObjectType(ba) == 0);

  // A bitmap from the program's resources: 4x2, the top row red green blue yellow, the bottom cyan magenta white black.
  g_pic = LoadBitmapA(inst, MAKEINTRESOURCEA(1));
  CHECK(16, g_pic != 0 && GetObjectA(g_pic, sizeof(bm), &bm) == sizeof(bm) && bm.bmWidth == 4 && bm.bmHeight == 2);
  {
    HBITMAP o2 = (HBITMAP)SelectObject(b, g_pic);

    CHECK(17, GetPixel(b, 0, 0) == RGB(255, 0, 0) && GetPixel(b, 3, 0) == RGB(255, 255, 0) && GetPixel(b, 0, 1) == RGB(0, 255, 255) && GetPixel(b, 3, 1) == RGB(0, 0, 0));
    SelectObject(b, o2);
  }
  CHECK(18, LoadBitmapA(inst, "PICTURE") != 0 && LoadBitmapA(inst, "NOSUCH") == 0);

  // A 32-bit bottom-up DIB section: the program writes the bits, the DC sees them.
  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = 4;
  bi.bmiHeader.biHeight = 3;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  dibs = CreateDIBSection(a, &bi, DIB_RGB_COLORS, (void **)&sect, 0, 0);
  CHECK(19, dibs != 0 && sect != 0);
  if (sect != 0) {
    sect[0] = 0x00123456;                           // memory row 0 is the bottom row
    sect[2 * 4 + 3] = 0x00FF0000;                   // the top row, last pixel: red
    SelectObject(a, dibs);
    CHECK(20, GetPixel(a, 0, 2) == RGB(0x12, 0x34, 0x56) && GetPixel(a, 3, 0) == RGB(255, 0, 0));
    SetRect(&r, 0, 0, 4, 1);
    br = CreateSolidBrush(RGB(0, 255, 0));
    FillRect(a, &r, br);                            // drawing into the DC shows in the program's memory
    CHECK(21, sect[2 * 4] == 0x0000FF00 && sect[2 * 4 + 3] == 0x0000FF00);
    DeleteObject(br);
    CHECK(22, GetObjectA(dibs, sizeof(bm), &bm) == sizeof(bm) && bm.bmBits == sect);
    SelectObject(a, old);
    DeleteObject(dibs);
  }

  // SetDIBits / GetDIBits / StretchDIBits with 24-bit data (2x1: red, blue).
  bi.bmiHeader.biWidth = 2;
  bi.bmiHeader.biHeight = 1;
  bi.bmiHeader.biBitCount = 24;
  ba = CreateCompatibleBitmap(a, 2, 1);
  CHECK(23, SetDIBits(a, ba, 0, 1, bits24, &bi, DIB_RGB_COLORS) == 1);
  SelectObject(a, ba);
  CHECK(24, GetPixel(a, 0, 0) == RGB(255, 0, 0) && GetPixel(a, 1, 0) == RGB(0, 0, 255));      // the bytes are B,G,R: red, then blue
  SelectObject(a, old);
  ZeroMemory(dibbuf, sizeof(dibbuf));
  bi.bmiHeader.biBitCount = 32;
  CHECK(25, GetDIBits(a, ba, 0, 1, dibbuf, &bi, DIB_RGB_COLORS) == 1 && (dibbuf[0] & 0xFFFFFF) == 0x00FF0000 && (dibbuf[1] & 0xFFFFFF) == 0x000000FF);
  bi.bmiHeader.biBitCount = 24;
  found = StretchDIBits(b, 0, 0, 4, 2, 0, 0, 2, 1, bits24, &bi, DIB_RGB_COLORS, SRCCOPY);
  CHECK(26, found != (int)GDI_ERROR && GetPixel(b, 0, 0) == RGB(255, 0, 0) && GetPixel(b, 3, 1) == RGB(0, 0, 255));
  DeleteObject(ba);

  // Text into a memory DC, transparent and opaque.
  bb = CreateCompatibleBitmap(a, 24, 8);
  old = (HBITMAP)SelectObject(a, bb);
  SetRect(&r, 0, 0, 24, 8);
  br = CreateSolidBrush(RGB(0, 0, 255));
  FillRect(a, &r, br);
  SetTextColor(a, RGB(255, 255, 255));
  SetBkMode(a, TRANSPARENT);
  TextOutA(a, 0, 0, "Hi", 2);
  found = 0;
  for (y = 0; y < 8; ++y) {
    for (x = 0; x < 16; ++x) {
      if (GetPixel(a, x, y) == RGB(255, 255, 255)) ++found;
    }
  }
  CHECK(27, found > 10 && GetPixel(a, 20, 4) == RGB(0, 0, 255));
  SetBkMode(a, OPAQUE);
  SetBkColor(a, RGB(255, 0, 0));
  TextOutA(a, 0, 0, "Hi", 2);
  CHECK(28, GetPixel(a, 15, 7) == RGB(255, 0, 0));        // the cell's background
  DeleteObject(br);
  SelectObject(a, old);
  DeleteObject(bb);

  // A monochrome bitmap draws in the text and background colours of the destination.
  {
    static const unsigned char mono[2] = {0xAA, 0x00};      // 8x1... rows are 16 bits: 10101010 then padding
    HBITMAP mb = CreateBitmap(8, 1, 1, 1, mono);

    SelectObject(a, mb);
    int mono_ok = GetPixel(a, 0, 0) == RGB(255, 255, 255) && GetPixel(a, 1, 0) == RGB(0, 0, 0);

    SelectObject(b, ba = CreateCompatibleBitmap(b, 8, 1));
    SetTextColor(b, RGB(255, 0, 0));
    SetBkColor(b, RGB(0, 255, 0));
    BitBlt(b, 0, 0, 8, 1, a, 0, 0, SRCCOPY);
    CHECK(29, mono_ok && GetPixel(b, 0, 0) == RGB(0, 255, 0) && GetPixel(b, 1, 0) == RGB(255, 0, 0));
    SelectObject(a, old);
    DeleteObject(mb);
  }
  DeleteDC(a);
  DeleteDC(b);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      HDC src = CreateCompatibleDC(dc);
      HBITMAP old = (HBITMAP)SelectObject(src, g_pic);
      BOOL ok;

      BitBlt(dc, 10, 10, 40, 30, g_mem, 0, 0, SRCCOPY);                          // the canvas at client (10,10)
      StretchBlt(dc, 100, 10, 80, 40, src, 0, 0, 4, 2, SRCCOPY);                 // the resource picture, 20x scaled
      StretchBlt(dc, 180, 60, -80, 40, src, 0, 0, 4, 2, SRCCOPY);                // and mirrored
      BitBlt(dc, 10, 100, 40, 30, g_mem, 0, 0, SRCCOPY);
      BitBlt(dc, 10, 100, 40, 30, g_mem, 0, 0, NOTSRCCOPY);                      // the inverse of the same bitmap over it
      ok = BitBlt(dc, 10, 150, 20, 20, g_mem, 0, 0, SRCINVERT);                  // reads the window's old pixels back: the background XOR red
      if (!ok || GetPixel(dc, 15, 155) != RGB(55, 220, 255)) FAIL(30);
      {
        HDC td = CreateCompatibleDC(dc);
        HBITMAP tb = CreateCompatibleBitmap(dc, 4, 4), told = (HBITMAP)SelectObject(td, tb);

        if (!BitBlt(td, 0, 0, 4, 4, dc, 0, 0, SRCCOPY) || GetPixel(td, 1, 1) != RGB(200, 220, 255)) FAIL(31);       // and a window is a source too
        SelectObject(td, told);
        DeleteObject(tb);
        DeleteDC(td);
      }
      PatBlt(dc, 100, 100, 20, 20, BLACKNESS);
      SelectObject(src, old);
      DeleteDC(src);
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
  make_canvas();
  api_checks(inst);
  if (g_pic == 0) {
    g_pic = LoadBitmapA(inst, MAKEINTRESOURCEA(1));
  }
  ZeroMemory(&wc, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hbrBackground = CreateSolidBrush(RGB(200, 220, 255));
  wc.lpszClassName = "WinCloneBitmaps";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneBitmaps", "Bitmaps", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 330, 0, 0, inst, 0);
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
