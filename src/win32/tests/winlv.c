// List view: a report view with three columns and twelve items (it needs a scroll bar) and a list view.
// The program checks the message API on a scratch list view first. The kernel self-test then clicks an item,
// moves the selection with the arrow keys, clicks the scroll bar's track and a column header (which makes the
// program sort the items); pixels show the header, the selection highlight and the scrolling, and the program
// counts the notifications it got. The exit code is a bit mask of the failures (0 = passed).
#include <windows.h>
#include <commctrl.h>

static unsigned g_bits;
static HWND g_lv, g_list;
static int g_changed, g_colclick, g_click, g_clicked_item = -2;

#define FAIL(n) (g_bits |= 1u << (n))
#define CHECK(n, cond) do { if (!(cond)) FAIL(n); } while (0)

static int CALLBACK by_param_desc(LPARAM a, LPARAM b, LPARAM l) { (void)l; return (int)(b - a); }
static int CALLBACK by_param_asc(LPARAM a, LPARAM b, LPARAM l) { (void)l; return (int)(a - b); }

static void add_item(HWND lv, int i, const char *text, const char *sub1, const char *sub2, LPARAM param) {
  LVITEMA it;

  ZeroMemory(&it, sizeof(it));
  it.mask = LVIF_TEXT | LVIF_PARAM;
  it.iItem = i;
  it.pszText = (LPSTR)text;
  it.lParam = param;
  SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&it);
  it.mask = LVIF_TEXT;
  it.iSubItem = 1;
  it.pszText = (LPSTR)sub1;
  SendMessageA(lv, LVM_SETITEMA, 0, (LPARAM)&it);
  it.iSubItem = 2;
  it.pszText = (LPSTR)sub2;
  SendMessageA(lv, LVM_SETITEMA, 0, (LPARAM)&it);
}

static void add_column(HWND lv, int i, const char *title, int width, int fmt) {
  LVCOLUMNA c;

  ZeroMemory(&c, sizeof(c));
  c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
  c.pszText = (LPSTR)title;
  c.cx = width;
  c.fmt = fmt;
  SendMessageA(lv, LVM_INSERTCOLUMNA, i, (LPARAM)&c);
}

// The message API on a list view nobody sees.
static void api_checks(HINSTANCE inst, HWND parent) {
  HWND t = CreateWindowExA(0, WC_LISTVIEWA, "", WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 200, 100, parent, (HMENU)299, inst, 0);
  char buf[32] = "";
  LVITEMA it;
  LVCOLUMNA c;
  LVHITTESTINFO hi;
  LVFINDINFOA fi;
  RECT r;

  CHECK(0, t != 0);
  add_column(t, 0, "Name", 100, LVCFMT_LEFT);
  add_column(t, 1, "Size", 60, LVCFMT_RIGHT);
  add_column(t, 2, "Type", 40, LVCFMT_LEFT);
  add_item(t, 0, "alpha", "1", "a", 10);
  add_item(t, 1, "beta", "22", "b", 20);
  add_item(t, 2, "gamma", "333", "c", 30);
  CHECK(1, SendMessageA(t, LVM_GETITEMCOUNT, 0, 0) == 3);
  ZeroMemory(&it, sizeof(it));
  it.iSubItem = 1;
  it.pszText = buf;
  it.cchTextMax = sizeof(buf);
  CHECK(2, SendMessageA(t, LVM_GETITEMTEXTA, 2, (LPARAM)&it) == 3 && buf[0] == '3' && buf[2] == '3');                 // the sub-item text of item 2
  ZeroMemory(&it, sizeof(it));
  it.mask = LVIF_PARAM | LVIF_TEXT;
  it.iItem = 1;
  it.pszText = buf;
  it.cchTextMax = sizeof(buf);
  CHECK(3, SendMessageA(t, LVM_GETITEMA, 0, (LPARAM)&it) && it.lParam == 20 && buf[0] == 'b');
  ZeroMemory(&it, sizeof(it));
  it.state = LVIS_SELECTED;
  it.stateMask = LVIS_SELECTED;
  SendMessageA(t, LVM_SETITEMSTATE, 1, (LPARAM)&it);
  CHECK(4, SendMessageA(t, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) == 1 && SendMessageA(t, LVM_GETSELECTEDCOUNT, 0, 0) == 1 &&
           SendMessageA(t, LVM_GETITEMSTATE, 1, LVIS_SELECTED) == LVIS_SELECTED && SendMessageA(t, LVM_GETITEMSTATE, 0, LVIS_SELECTED) == 0);
  ZeroMemory(&fi, sizeof(fi));
  fi.flags = LVFI_STRING;
  fi.psz = "GAMMA";
  CHECK(5, SendMessageA(t, LVM_FINDITEMA, (WPARAM)-1, (LPARAM)&fi) == 2);                                              // case-insensitive
  fi.flags = LVFI_PARAM;
  fi.lParam = 10;
  CHECK(6, SendMessageA(t, LVM_FINDITEMA, (WPARAM)-1, (LPARAM)&fi) == 0 && SendMessageA(t, LVM_FINDITEMA, 0, (LPARAM)&fi) == -1);
  r.left = LVIR_BOUNDS;
  CHECK(7, SendMessageA(t, LVM_GETITEMRECT, 1, (LPARAM)&r) && r.top == 32 && r.bottom == 46 && r.left == 0 && r.right == 200);   // below the 18-pixel header
  hi.pt.x = 10;
  hi.pt.y = 40;
  CHECK(8, SendMessageA(t, LVM_HITTEST, 0, (LPARAM)&hi) == 1 && hi.iItem == 1 && (hi.flags & LVHT_ONITEM));
  hi.pt.y = 5;
  CHECK(9, SendMessageA(t, LVM_HITTEST, 0, (LPARAM)&hi) == -1 && hi.flags == LVHT_NOWHERE);                            // the header is not an item
  hi.pt.y = 80;
  CHECK(10, SendMessageA(t, LVM_HITTEST, 0, (LPARAM)&hi) == -1);                                                       // below the last item
  SendMessageA(t, LVM_SORTITEMS, 0, (LPARAM)by_param_desc);
  ZeroMemory(&it, sizeof(it));
  it.iSubItem = 0;
  it.pszText = buf;
  it.cchTextMax = sizeof(buf);
  SendMessageA(t, LVM_GETITEMTEXTA, 0, (LPARAM)&it);
  CHECK(11, buf[0] == 'g' && SendMessageA(t, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) == 1);                          // "beta" is still selected, in the middle
  SendMessageA(t, LVM_SORTITEMS, 0, (LPARAM)by_param_asc);
  CHECK(12, SendMessageA(t, LVM_DELETEITEM, 0, 0) && SendMessageA(t, LVM_GETITEMCOUNT, 0, 0) == 2 && SendMessageA(t, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) == 0);
  CHECK(13, SendMessageA(t, LVM_GETCOLUMNWIDTH, 1, 0) == 60 && SendMessageA(t, LVM_SETCOLUMNWIDTH, 1, 90) && SendMessageA(t, LVM_GETCOLUMNWIDTH, 1, 0) == 90);
  ZeroMemory(&c, sizeof(c));
  c.mask = LVCF_TEXT | LVCF_FMT;
  c.pszText = buf;
  c.cchTextMax = sizeof(buf);
  CHECK(14, SendMessageA(t, LVM_GETCOLUMNA, 1, (LPARAM)&c) && buf[0] == 'S' && c.fmt == LVCFMT_RIGHT);
  add_column(t, 1, "New", 50, LVCFMT_LEFT);                                                                           // a column in the middle: the sub-items move
  ZeroMemory(&it, sizeof(it));
  it.iSubItem = 2;
  it.pszText = buf;
  it.cchTextMax = sizeof(buf);
  SendMessageA(t, LVM_GETITEMTEXTA, 0, (LPARAM)&it);
  CHECK(15, buf[0] == '2');
  SendMessageA(t, LVM_DELETECOLUMN, 1, 0);
  SendMessageA(t, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
  ZeroMemory(&it, sizeof(it));
  it.state = 2u << 12;
  it.stateMask = LVIS_STATEIMAGEMASK;
  SendMessageA(t, LVM_SETITEMSTATE, 0, (LPARAM)&it);
  CHECK(16, SendMessageA(t, LVM_GETITEMSTATE, 0, LVIS_STATEIMAGEMASK) == (2u << 12) && (SendMessageA(t, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0) & LVS_EX_CHECKBOXES));
  SendMessageA(t, LVM_DELETEALLITEMS, 0, 0);
  CHECK(17, SendMessageA(t, LVM_GETITEMCOUNT, 0, 0) == 0);
  DestroyWindow(t);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      HINSTANCE inst = GetModuleHandleA(0);
      char name[16];
      int i;

      InitCommonControls();
      api_checks(inst, h);
      g_lv = CreateWindowExA(0, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 10, 10, 300, 120, h, (HMENU)201, inst, 0);
      add_column(g_lv, 0, "Name", 100, LVCFMT_LEFT);
      add_column(g_lv, 1, "Size", 80, LVCFMT_RIGHT);
      add_column(g_lv, 2, "Type", 80, LVCFMT_LEFT);
      for (i = 0; i < 12; ++i) {
        name[0] = 'F'; name[1] = 'i'; name[2] = 'l'; name[3] = 'e';
        name[4] = (char)('0' + i / 10);
        name[5] = (char)('0' + i % 10);
        name[6] = 0;
        add_item(g_lv, i, name, "1 KB", "Doc", i);
      }
      g_list = CreateWindowExA(0, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | LVS_LIST, 10, 140, 200, 60, h, (HMENU)202, inst, 0);
      for (i = 0; i < 6; ++i) {
        name[0] = 'L'; name[1] = (char)('0' + i); name[2] = 0;
        add_item(g_list, i, name, "", "", i);
      }
      return 0;
    }
    case WM_NOTIFY: {
      const NMHDR *nm = (const NMHDR *)lp;

      if (nm->hwndFrom == g_lv) {
        if (nm->code == LVN_ITEMCHANGED) ++g_changed;
        if (nm->code == NM_CLICK) {
          ++g_click;
          g_clicked_item = ((const NMITEMACTIVATE *)lp)->iItem;
        }
        if (nm->code == LVN_COLUMNCLICK) {
          ++g_colclick;
          SendMessageA(g_lv, LVM_SORTITEMS, 0, (LPARAM)by_param_desc);
        }
      }
      return 0;
    }
    case WM_CLOSE: {
      char buf[16] = "";
      LVITEMA it;
      int sel;

      CHECK(18, g_colclick == 1 && g_click == 1 && g_clicked_item == 2);
      CHECK(19, g_changed == 10);                                                     // a click (select + focus) and two arrow keys (four changes each)
      ZeroMemory(&it, sizeof(it));
      it.pszText = buf;
      it.cchTextMax = sizeof(buf);
      SendMessageA(g_lv, LVM_GETITEMTEXTA, 0, (LPARAM)&it);
      CHECK(20, buf[4] == '1' && buf[5] == '1');                                     // sorted descending: File11 first
      sel = (int)SendMessageA(g_lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
      SendMessageA(g_lv, LVM_GETITEMTEXTA, sel, (LPARAM)&it);
      CHECK(21, sel == 7 && buf[5] == '4' && SendMessageA(g_lv, LVM_GETSELECTEDCOUNT, 0, 0) == 1);         // File04, moved along with its data
      CHECK(22, SendMessageA(g_lv, LVM_GETTOPINDEX, 0, 0) > 0);                       // the scroll bar's page click scrolled
      CHECK(23, SendMessageA(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) == 4);   // a click in the second column of the list view
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
  wc.lpszClassName = "WinCloneListView";
  if (!RegisterClassA(&wc)) {
    return 1;
  }
  hwnd = CreateWindowExA(0, "WinCloneListView", "List view", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 280, 0, 0, inst, 0);
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
