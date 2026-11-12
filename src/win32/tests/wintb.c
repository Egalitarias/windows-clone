// Image lists, the tool bar and tool tips. The program checks the message API of each on controls nobody
// sees (an image list drawn into a memory DC, a hidden tool bar and a hidden tip control), then shows a flat
// tool bar with eight buttons (push, check, radio group, a disabled one, a drop-down) and a static control with a
// tip of its own. The kernel self-test clicks buttons, hovers over a button and over the static control and
// looks at pixels: images, hot / checked / pressed buttons, the tips appearing and going away. The program
// counts what its window got. The exit code is a bit mask of the failures (0 = passed).
#include <windows.h>
#include <commctrl.h>

static unsigned g_bits;
static HWND g_tb, g_static, g_tip2;
static int g_cmd[8], g_last_cmd, g_dropdown, g_dropdown_item, g_need[8], g_pop;

#define FAIL(n) (g_bits |= 1u << ((n) % 32))    // (more checks than bits: the numbers wrap)
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

// A strip of `n` squares of `s` pixels in the given colours, with a magenta pixel in the top left of each.
static HBITMAP strip(HDC dc, int n, int s, const COLORREF *c) {
  HBITMAP bm = CreateCompatibleBitmap(dc, n * s, s);
  HBITMAP old = (HBITMAP)SelectObject(dc, bm);
  int i;

  for (i = 0; i < n; ++i) {
    RECT r;
    HBRUSH br = CreateSolidBrush(c[i]);

    r.left = i * s;
    r.top = 0;
    r.right = (i + 1) * s;
    r.bottom = s;
    FillRect(dc, &r, br);
    DeleteObject(br);
    SetPixel(dc, i * s, 0, RGB(255, 0, 255));
  }
  SelectObject(dc, old);
  return bm;
}

static COLORREF pix(HDC dc, HBITMAP bm, int x, int y) {
  HBITMAP old = (HBITMAP)SelectObject(dc, bm);
  COLORREF c = GetPixel(dc, x, y);

  SelectObject(dc, old);
  return c;
}

static void il_checks(void) {
  HDC dc = CreateCompatibleDC(0);
  COLORREF two[2] = {RGB(255, 0, 0), RGB(0, 0, 255)};
  HBITMAP s = strip(dc, 2, 8, two), canvas = CreateCompatibleBitmap(dc, 20, 12);
  HIMAGELIST il = ImageList_Create(8, 8, ILC_COLOR32 | ILC_MASK, 0, 2), ov;
  HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
  RECT all = {0, 0, 20, 12};
  HBITMAP old = (HBITMAP)SelectObject(dc, canvas);
  int w = 0, h = 0;

  CHECK(0, il != 0 && ImageList_GetImageCount(il) == 0);
  CHECK(1, ImageList_AddMasked(il, s, RGB(255, 0, 255)) == 0 && ImageList_GetImageCount(il) == 2);
  CHECK(2, ImageList_GetIconSize(il, &w, &h) && w == 8 && h == 8);
  FillRect(dc, &all, white);
  ImageList_Draw(il, 0, dc, 1, 1, ILD_NORMAL);
  CHECK(3, GetPixel(dc, 1, 1) == RGB(255, 255, 255) && GetPixel(dc, 4, 4) == RGB(255, 0, 0));      // the key colour is transparent
  CHECK(4, ImageList_SetBkColor(il, RGB(0, 255, 0)) == CLR_NONE && ImageList_GetBkColor(il) == RGB(0, 255, 0));
  ImageList_Draw(il, 0, dc, 1, 1, ILD_NORMAL);
  CHECK(5, GetPixel(dc, 1, 1) == RGB(0, 255, 0) && GetPixel(dc, 4, 4) == RGB(255, 0, 0));          // drawn over the background colour
  FillRect(dc, &all, white);
  ImageList_Draw(il, 0, dc, 1, 1, ILD_TRANSPARENT);
  CHECK(6, GetPixel(dc, 1, 1) == RGB(255, 255, 255));
  ImageList_Draw(il, 0, dc, 10, 1, ILD_MASK);
  CHECK(7, GetPixel(dc, 13, 4) == RGB(0, 0, 0) && GetPixel(dc, 10, 1) == RGB(255, 255, 255));      // the mask: opaque black, transparent white
  FillRect(dc, &all, white);
  ImageList_Draw(il, 1, dc, 1, 1, ILD_SELECTED);
  CHECK(8, GetPixel(dc, 4, 4) != RGB(0, 0, 255) && GetPixel(dc, 4, 4) != RGB(255, 255, 255));      // blended with the highlight colour
  CHECK(9, ImageList_Remove(il, 0) && ImageList_GetImageCount(il) == 1);
  ImageList_SetBkColor(il, CLR_NONE);
  FillRect(dc, &all, white);
  ImageList_Draw(il, 0, dc, 1, 1, ILD_NORMAL);
  CHECK(10, GetPixel(dc, 4, 4) == RGB(0, 0, 255));                                                   // the second image moved up
  ov = ImageList_Duplicate(il);
  CHECK(11, ov != 0 && ImageList_GetImageCount(ov) == 1 && ImageList_SetImageCount(ov, 3) && ImageList_GetImageCount(ov) == 3);
  CHECK(12, ImageList_SetOverlayImage(ov, 0, 1) && !ImageList_SetOverlayImage(ov, 5, 2));
  {
    HBITMAP red = strip(dc, 1, 8, two);

    ImageList_Replace(ov, 1, red, 0);
    FillRect(dc, &all, white);
    ImageList_Draw(ov, 1, dc, 1, 1, INDEXTOOVERLAYMASK(1) | ILD_NORMAL);
    CHECK(13, GetPixel(dc, 4, 4) == RGB(0, 0, 255) && GetPixel(dc, 1, 1) == RGB(255, 0, 255));       // the overlay (blue) is over the red image; Replace used no key, so magenta stays
    DeleteObject(red);
  }
  SelectObject(dc, old);
  ImageList_Destroy(ov);
  ImageList_Destroy(il);
  CHECK(14, !ImageList_Destroy(il));
  DeleteObject(s);
  DeleteObject(canvas);
  DeleteObject(white);
  DeleteDC(dc);
  (void)pix;
}

static HIMAGELIST make_list(void) {
  HDC dc = CreateCompatibleDC(0);
  COLORREF c[4] = {RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(255, 255, 0)};
  HBITMAP s = strip(dc, 4, 16, c);
  HIMAGELIST il = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 4, 4);

  ImageList_AddMasked(il, s, RGB(255, 0, 255));
  DeleteObject(s);
  DeleteDC(dc);
  return il;
}

static void tb_checks(HINSTANCE inst, HWND parent, HIMAGELIST il) {
  HWND t = CreateWindowExA(0, TOOLBARCLASSNAMEA, "", WS_CHILD | CCS_NOPARENTALIGN | CCS_NORESIZE, 0, 300, 400, 30, parent, (HMENU)298, inst, 0);
  TBBUTTON b[4];
  RECT r;
  POINT p;
  TBBUTTONINFOA bi;
  char buf[32];
  const char *strs = "One\0Two\0";

  CHECK(15, t != 0);
  SendMessageA(t, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  SendMessageA(t, TB_SETIMAGELIST, 0, (LPARAM)il);
  CHECK(16, SendMessageA(t, TB_ADDSTRINGA, 0, (LPARAM)strs) == 0);
  ZeroMemory(b, sizeof(b));
  b[0].iBitmap = 0; b[0].idCommand = 1; b[0].fsState = TBSTATE_ENABLED; b[0].fsStyle = BTNS_CHECKGROUP; b[0].iString = -1;
  b[1].iBitmap = 1; b[1].idCommand = 2; b[1].fsState = TBSTATE_ENABLED; b[1].fsStyle = BTNS_CHECKGROUP; b[1].iString = -1;
  b[2].iBitmap = 12; b[2].fsStyle = BTNS_SEP; b[2].fsState = TBSTATE_ENABLED;
  b[3].iBitmap = 2; b[3].idCommand = 3; b[3].fsState = TBSTATE_ENABLED; b[3].fsStyle = BTNS_BUTTON; b[3].iString = -1;
  CHECK(17, SendMessageA(t, TB_ADDBUTTONSA, 4, (LPARAM)b) && SendMessageA(t, TB_BUTTONCOUNT, 0, 0) == 4);
  CHECK(18, SendMessageA(t, TB_COMMANDTOINDEX, 3, 0) == 3 && SendMessageA(t, TB_COMMANDTOINDEX, 99, 0) == -1);
  SendMessageA(t, TB_GETITEMRECT, 1, (LPARAM)&r);
  CHECK(19, r.left == 24 && r.right == 48 && r.top == 2 && r.bottom == 26);                         // 16x16 images: 24x24 buttons under the divider
  SendMessageA(t, TB_GETITEMRECT, 2, (LPARAM)&r);
  CHECK(20, r.left == 48 && r.right == 60);                                                          // a separator 12 wide
  p.x = 55; p.y = 10;
  CHECK(21, SendMessageA(t, TB_HITTEST, 0, (LPARAM)&p) == -3);                                      // over the separator: -(index + 1)
  p.x = 30;
  CHECK(22, SendMessageA(t, TB_HITTEST, 0, (LPARAM)&p) == 1);
  SendMessageA(t, TB_CHECKBUTTON, 1, MAKELPARAM(TRUE, 0));
  SendMessageA(t, TB_CHECKBUTTON, 2, MAKELPARAM(TRUE, 0));
  CHECK(23, SendMessageA(t, TB_ISBUTTONCHECKED, 2, 0) && !SendMessageA(t, TB_ISBUTTONCHECKED, 1, 0));   // a check group: one at a time
  SendMessageA(t, TB_ENABLEBUTTON, 3, MAKELPARAM(FALSE, 0));
  CHECK(24, !SendMessageA(t, TB_ISBUTTONENABLED, 3, 0) && (SendMessageA(t, TB_GETSTATE, 3, 0) & TBSTATE_ENABLED) == 0);
  ZeroMemory(&bi, sizeof(bi));
  bi.cbSize = sizeof(bi);
  bi.dwMask = TBIF_TEXT | TBIF_SIZE | TBIF_COMMAND;
  bi.pszText = (LPSTR)"Wide";
  bi.cx = 50;
  bi.idCommand = 33;
  CHECK(25, SendMessageA(t, TB_SETBUTTONINFOA, 3, (LPARAM)&bi) && SendMessageA(t, TB_GETBUTTONTEXTA, 33, (LPARAM)buf) == 4);
  SendMessageA(t, TB_GETITEMRECT, 3, (LPARAM)&r);
  CHECK(26, r.right - r.left == 50 && buf[0] == 'W');
  SendMessageA(t, TB_HIDEBUTTON, 1, MAKELPARAM(TRUE, 0));
  SendMessageA(t, TB_GETITEMRECT, 2, (LPARAM)&r);
  CHECK(27, r.left == 24);                                                                           // the hidden button's space is gone
  CHECK(28, SendMessageA(t, TB_DELETEBUTTON, 0, 0) && SendMessageA(t, TB_BUTTONCOUNT, 0, 0) == 3);
  DestroyWindow(t);
}

static void tip_checks(HWND parent) {
  HWND tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, 0, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, parent, 0, GetModuleHandleA(0), 0);
  TTTOOLINFOA ti, out;
  TTHITTESTINFOA hi;
  char buf[80];

  CHECK(29, tip != 0 && SendMessageA(tip, TTM_GETTOOLCOUNT, 0, 0) == 0);
  ZeroMemory(&ti, sizeof(ti));
  ti.cbSize = sizeof(ti);
  ti.hwnd = parent;
  ti.uId = 7;
  SetRect(&ti.rect, 10, 10, 50, 30);
  ti.lpszText = (LPSTR)"Seven";
  CHECK(30, SendMessageA(tip, TTM_ADDTOOLA, 0, (LPARAM)&ti) && SendMessageA(tip, TTM_GETTOOLCOUNT, 0, 0) == 1);
  out = ti;
  out.lpszText = buf;
  buf[0] = 0;
  CHECK(31, SendMessageA(tip, TTM_GETTOOLINFOA, 0, (LPARAM)&out) && buf[0] == 'S' && buf[4] == 'n');
  hi.hwnd = parent;
  hi.pt.x = 20;
  hi.pt.y = 20;
  hi.ti.cbSize = sizeof(hi.ti);
  CHECK(32, SendMessageA(tip, TTM_HITTESTA, 0, (LPARAM)&hi) && hi.ti.uId == 7);
  hi.pt.x = 80;
  CHECK(33, !SendMessageA(tip, TTM_HITTESTA, 0, (LPARAM)&hi));
  SendMessageA(tip, TTM_SETDELAYTIME, TTDT_INITIAL, 321);
  CHECK(34, SendMessageA(tip, TTM_GETDELAYTIME, TTDT_INITIAL, 0) == 321);
  SendMessageA(tip, TTM_SETMAXTIPWIDTH, 0, 120);
  CHECK(35, SendMessageA(tip, TTM_GETMAXTIPWIDTH, 0, 0) == 120);
  SendMessageA(tip, TTM_DELTOOLA, 0, (LPARAM)&ti);
  CHECK(36, SendMessageA(tip, TTM_GETTOOLCOUNT, 0, 0) == 0);
  DestroyWindow(tip);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      HINSTANCE inst = ((CREATESTRUCTA *)lp)->hInstance;
      HIMAGELIST il;
      TBBUTTON b[8];
      TTTOOLINFOA ti;
      HWND tip;
      int i;

      il_checks();
      il = make_list();
      tb_checks(inst, h, il);
      tip_checks(h);
      g_tb = CreateWindowExA(0, TOOLBARCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS, 0, 0, 0, 0, h, (HMENU)1, inst, 0);
      SendMessageA(g_tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
      SendMessageA(g_tb, TB_SETIMAGELIST, 0, (LPARAM)il);
      ZeroMemory(b, sizeof(b));
      for (i = 0; i < 8; ++i) {
        b[i].iBitmap = i % 4;
        b[i].idCommand = 101 + i;
        b[i].fsState = TBSTATE_ENABLED;
        b[i].iString = -1;
      }
      b[2].fsStyle = BTNS_SEP; b[2].iBitmap = 8; b[2].idCommand = 0;
      b[3].fsStyle = BTNS_CHECK; b[3].idCommand = 103;
      b[4].fsStyle = BTNS_CHECKGROUP; b[4].idCommand = 104;
      b[5].fsStyle = BTNS_CHECKGROUP; b[5].idCommand = 105; b[5].iBitmap = 0;
      b[6].fsState = 0; b[6].idCommand = 106;
      b[7].fsStyle = BTNS_DROPDOWN; b[7].idCommand = 107;
      CHECK(37, SendMessageA(g_tb, TB_ADDBUTTONSA, 8, (LPARAM)b) != 0);
      SendMessageA(g_tb, TB_AUTOSIZE, 0, 0);
      tip = (HWND)SendMessageA(g_tb, TB_GETTOOLTIPS, 0, 0);
      CHECK(38, tip != 0);
      SendMessageA(tip, TTM_SETDELAYTIME, TTDT_INITIAL, 100);
      g_static = CreateWindowExA(0, "STATIC", "hover me", WS_CHILD | WS_VISIBLE | SS_CENTER, 200, 120, 80, 20, h, (HMENU)2, inst, 0);
      g_tip2 = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, 0, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, h, 0, inst, 0);
      SendMessageA(g_tip2, TTM_SETDELAYTIME, TTDT_INITIAL, 100);
      ZeroMemory(&ti, sizeof(ti));
      ti.cbSize = sizeof(ti);
      ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
      ti.hwnd = h;
      ti.uId = (UINT_PTR)g_static;
      ti.lpszText = (LPSTR)"Static tip";
      CHECK(39, SendMessageA(g_tip2, TTM_ADDTOOLA, 0, (LPARAM)&ti));
      // a tab control and a list view that use the same image list
      {
        HWND tab = CreateWindowExA(0, WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE, 10, 40, 150, 60, h, (HMENU)3, inst, 0);
        HWND lv = CreateWindowExA(0, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | LVS_REPORT, 10, 110, 180, 70, h, (HMENU)4, inst, 0);
        TCITEMA ti2;
        LVITEMA li;
        LVCOLUMNA col;
        static const char *tabs[2] = {"A", "B"};
        static const char *names[3] = {"One", "Two", "Three"};

        CHECK(47, SendMessageA(tab, TCM_SETIMAGELIST, 0, (LPARAM)il) == 0 && SendMessageA(tab, TCM_GETIMAGELIST, 0, 0) == (LRESULT)il);
        for (i = 0; i < 2; ++i) {
          ZeroMemory(&ti2, sizeof(ti2));
          ti2.mask = TCIF_TEXT | TCIF_IMAGE;
          ti2.pszText = (LPSTR)tabs[i];
          ti2.iImage = i + 1;
          SendMessageA(tab, TCM_INSERTITEMA, i, (LPARAM)&ti2);
        }
        ZeroMemory(&ti2, sizeof(ti2));
        ti2.mask = TCIF_IMAGE;
        SendMessageA(tab, TCM_GETITEMA, 1, (LPARAM)&ti2);
        CHECK(48, ti2.iImage == 2);
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPSTR)"Name";
        col.cx = 120;
        SendMessageA(lv, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
        CHECK(49, SendMessageA(lv, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)il) == 0 && SendMessageA(lv, LVM_GETIMAGELIST, LVSIL_SMALL, 0) == (LRESULT)il);
        for (i = 0; i < 3; ++i) {
          ZeroMemory(&li, sizeof(li));
          li.mask = LVIF_TEXT | LVIF_IMAGE;
          li.iItem = i;
          li.pszText = (LPSTR)names[i];
          li.iImage = i;
          SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&li);
        }
        ZeroMemory(&li, sizeof(li));
        li.mask = LVIF_IMAGE;
        li.iItem = 2;
        SendMessageA(lv, LVM_GETITEMA, 0, (LPARAM)&li);
        CHECK(50, li.iImage == 2);
        SendMessageA(lv, LVM_GETITEMRECT, 1, (LPARAM)&ti.rect);
        CHECK(51, ti.rect.bottom - ti.rect.top == 17 && ti.rect.top == 35);                // the 16 pixel images made the rows 17 high
      }
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wp) >= 101 && LOWORD(wp) <= 108) {
        ++g_cmd[LOWORD(wp) - 101];
        g_last_cmd = LOWORD(wp);
        InvalidateRect(h, 0, TRUE);
      }
      return 0;
    case WM_NOTIFY: {
      NMHDR *nm = (NMHDR *)lp;

      if (nm->code == TBN_DROPDOWN) {
        ++g_dropdown;
        g_dropdown_item = ((NMTOOLBARA *)lp)->iItem;
      } else if (nm->code == TTN_GETDISPINFOA && nm->hwndFrom != g_tip2) {
        NMTTDISPINFOA *d = (NMTTDISPINFOA *)lp;

        if (nm->idFrom >= 101 && nm->idFrom <= 108) ++g_need[nm->idFrom - 101];
        d->lpszText = (LPSTR)"New file";
      } else if (nm->code == TTN_POP) {
        ++g_pop;
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);

      if (g_last_cmd != 0) {
        RECT r = {200, 60, 260, 100};
        HBRUSH br = CreateSolidBrush(RGB(g_last_cmd, 100, 50));

        FillRect(dc, &r, br);
        DeleteObject(br);
      }
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CLOSE:
      CHECK(40, g_cmd[1] == 1 && g_cmd[2] == 1 && g_cmd[3] == 1 && g_cmd[4] == 1 && g_cmd[0] == 0);   // 102, 103, 104, 105 once each; New never
      CHECK(41, g_cmd[5] == 0 && g_cmd[6] == 1);                                                         // the disabled 106 never; 107 (the drop-down's button part) once
      CHECK(42, g_dropdown == 1 && g_dropdown_item == 107);                                              // its arrow: TBN_DROPDOWN, no command
      CHECK(43, g_need[0] > 0);                                                                          // the parent was asked for the text of the first button
      CHECK(44, g_pop >= 2);                                                                             // both tips went away (TTN_POP)
      CHECK(45, SendMessageA(g_tb, TB_ISBUTTONCHECKED, 104, 0) == 0 && SendMessageA(g_tb, TB_ISBUTTONCHECKED, 105, 0) == 1);
      CHECK(46, SendMessageA(g_tb, TB_ISBUTTONCHECKED, 103, 0) == 1);
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
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = "WinCloneToolbar";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneToolbar", "Tool bar", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 280, 0, 0, inst, 0);
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
