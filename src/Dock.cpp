#include "wintop.h"
#include <algorithm>

// ─── 聚合条常量 ───
#define DOCK_CELL_W      240      // 单元格总宽
#define DOCK_CELL_H      176      // 单元格总高（含标题栏/边框）
#define DOCK_PADDING     8
#define DOCK_DRAG_STRIP  24       // 顶部拖动条高度
#define DOCK_LEGEND_H    18       // 底部色相图例高度
#define DOCK_MIN_CELL_W  150      // 单元格最小宽（便签很多时收缩）
#define DOCK_BAR_BG      RGB(24, 26, 32)
#define DOCK_BAR_BORDER  RGB(90, 100, 140)

static HWND g_hDock = nullptr;
static bool g_dockEnabled = false;
static int  g_dockX = 0;          // 悬浮条左上角（屏幕坐标）
static int  g_dockY = 0;

// 上次应用的排序序列（防抖：顺序没变就不重复 SetWindowPos）
static std::vector<HWND> g_appliedOrder;

// 拖拽状态
static POINT g_dragStart = {0, 0};
static POINT g_dragBarOrig = {0, 0};
static bool  g_maybeDrag = false;
static bool  g_dragging = false;

// 排序键：暂停/停滞的便签视为低频（最左）
static float SortKey(const StickyNote& note) {
    return (note.paused || note.isStale) ? 0.0f : note.activityScore;
}

static void CreateDockBar();   // 定义在文件后部

static void ClampDockPos() {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (g_dockX < 0) g_dockX = 0;
    if (g_dockY < 0) g_dockY = 0;
    if (g_dockX > sw - 120) g_dockX = sw - 120;
    if (g_dockY > sh - 120) g_dockY = sh - 120;
}

// ─── 布局：按更新频率升序（低频左 → 高频右）铺排悬浮条与便签 ───
// force=true 时即使顺序未变也整体重排（拖动悬浮条时使用）
static void DockLayout(bool force) {
    if (!g_dockEnabled || !g_hDock) return;

    std::vector<StickyNote*> cells;
    for (auto& kv : g_stickyNotes) {
        if (kv.second->docked) cells.push_back(kv.second.get());
    }
    int n = (int)cells.size();
    if (n == 0) {
        g_appliedOrder.clear();
        ShowWindow(g_hDock, SW_HIDE);
        return;
    }
    ShowWindow(g_hDock, SW_SHOW);

    std::sort(cells.begin(), cells.end(), [](const StickyNote* a, const StickyNote* b) {
        return SortKey(*a) < SortKey(*b);
    });

    // 防抖：排序序列与上次一致则无需重排（除非 force）
    bool sameOrder = !force && ((int)g_appliedOrder.size() == n);
    if (sameOrder) {
        for (int i = 0; i < n; ++i) {
            if (g_appliedOrder[i] != cells[i]->hwnd) { sameOrder = false; break; }
        }
    }
    if (sameOrder) return;

    // 单元格宽度：铺满一行，上限 240、下限 150
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int avail = screenW - 2 * DOCK_PADDING;
    int cellW = avail / n;
    if (cellW > DOCK_CELL_W) cellW = DOCK_CELL_W;
    if (cellW < DOCK_MIN_CELL_W) cellW = DOCK_MIN_CELL_W;

    int barW = n * cellW + 2 * DOCK_PADDING;
    int barH = DOCK_DRAG_STRIP + DOCK_PADDING + DOCK_CELL_H +
               DOCK_PADDING + DOCK_LEGEND_H + DOCK_PADDING;

    ClampDockPos();
    SetWindowPos(g_hDock, HWND_TOPMOST, g_dockX, g_dockY, barW, barH, SWP_NOACTIVATE);

    int cellY = g_dockY + DOCK_DRAG_STRIP + DOCK_PADDING;
    g_appliedOrder.clear();
    for (int i = 0; i < n; ++i) {
        int x = g_dockX + DOCK_PADDING + i * cellW;
        StickyNote* p = cells[i];
        p->dockSlotRect = { x, cellY, x + cellW, cellY + DOCK_CELL_H };
        SetWindowPos(p->hwnd, HWND_TOPMOST, x, cellY, cellW, DOCK_CELL_H, SWP_NOACTIVATE);
        g_appliedOrder.push_back(p->hwnd);
    }
}

// ─── 入坞：记录恢复几何/样式，去掉缩放与最小化按钮，保留标题栏作拖动把手 ───
void DockNote(StickyNote& note) {
    if (note.docked) return;
    if (!g_dockEnabled) {
        g_dockEnabled = true;
        CreateDockBar();
    }

    RECT rc;
    GetWindowRect(note.hwnd, &rc);
    note.dockRestoreRect = rc;
    note.savedStyle = (LONG)GetWindowLongPtr(note.hwnd, GWL_STYLE);
    note.savedExStyle = (LONG)GetWindowLongPtr(note.hwnd, GWL_EXSTYLE);

    SetWindowLongPtr(note.hwnd, GWL_STYLE,
                     note.savedStyle & ~(WS_THICKFRAME | WS_MINIMIZEBOX));
    note.docked = true;
    SetWindowPos(note.hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    DockLayout(false);
}

// ─── 出坞：恢复样式；restorePos=true 恢复入坞前位置尺寸 ───
void UndockNote(StickyNote& note, bool restorePos) {
    if (!note.docked) return;
    note.docked = false;
    if (note.savedStyle) SetWindowLongPtr(note.hwnd, GWL_STYLE, note.savedStyle);
    if (note.savedExStyle) SetWindowLongPtr(note.hwnd, GWL_EXSTYLE, note.savedExStyle);

    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;
    if (restorePos) {
        SetWindowPos(note.hwnd, nullptr,
                     note.dockRestoreRect.left, note.dockRestoreRect.top,
                     note.dockRestoreRect.right - note.dockRestoreRect.left,
                     note.dockRestoreRect.bottom - note.dockRestoreRect.top,
                     flags);
    } else {
        SetWindowPos(note.hwnd, nullptr, 0, 0, 0, 0,
                     flags | SWP_NOMOVE | SWP_NOSIZE);
    }
    DockLayout(false);
}

// ─── 对外：重新布局（顺序变化才动） ───
void DockRelayout() {
    DockLayout(false);
}

bool IsDockEnabled() {
    return g_dockEnabled;
}

// ─── 切换聚合模式：一键把所有便签吸入 / 恢复悬浮 ───
void ToggleDock() {
    if (g_dockEnabled) {
        for (auto& kv : g_stickyNotes) {
            UndockNote(*kv.second, true);
        }
        g_dockEnabled = false;
        g_appliedOrder.clear();
        if (g_hDock) {
            DestroyWindow(g_hDock);
            g_hDock = nullptr;
        }
    } else {
        g_dockEnabled = true;
        CreateDockBar();
        for (auto& kv : g_stickyNotes) {
            DockNote(*kv.second);
        }
        DockLayout(true);
    }
}

// ─── 聚合条右键菜单 ───
static void ShowDockMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, IDM_TRAY_TOGGLE_DOCK, L"退出聚合模式（恢复悬浮）");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, IDM_DOCK_EJECT_ALL, L"全部移出聚合栏");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

// ─── 聚合条窗口过程 ───
static LRESULT CALLBACK DockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            // 双缓冲
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

            // 背景
            HBRUSH bg = CreateSolidBrush(DOCK_BAR_BG);
            FillRect(mem, &rc, bg);
            DeleteObject(bg);

            // 圆角边框
            HPEN pen = CreatePen(PS_SOLID, 1, DOCK_BAR_BORDER);
            SelectObject(mem, pen);
            SelectObject(mem, GetStockObject(NULL_BRUSH));
            RoundRect(mem, 1, 1, rc.right - 1, rc.bottom - 1, 10, 10);
            DeleteObject(pen);

            // 顶部拖动条：拖拽把手圆点 + 说明文字
            RECT title = { 0, 0, rc.right, DOCK_DRAG_STRIP };
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(160, 170, 200));
            HFONT f = (HFONT)SelectObject(mem, g_hFont);
            DrawText(mem, L"聚合栏 · 按更新频率排序（低频左 → 高频右）", -1, &title,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            HBRUSH dot = CreateSolidBrush(RGB(120, 130, 160));
            for (int i = 0; i < 3; ++i) {
                RECT d = { 10 + i * 8, 8, 16 + i * 8, 14 };
                Ellipse(mem, d.left, d.top, d.right, d.bottom);
            }
            DeleteObject(dot);
            SelectObject(mem, f);

            // 底部色相图例：低频蓝 → 高频红
            int legendY = rc.bottom - DOCK_LEGEND_H;
            for (int x = 0; x < rc.right; x += 4) {
                float a = (float)x / (float)rc.right;
                HBRUSH cb = CreateSolidBrush(HueFromActivity(a));
                int x2 = x + 4 < rc.right ? x + 4 : rc.right;
                RECT seg = { x, legendY, x2, legendY + DOCK_LEGEND_H };
                FillRect(mem, &seg, cb);
                DeleteObject(cb);
            }
            SelectObject(mem, g_hFont);
            SetTextColor(mem, RGB(255, 255, 255));
            RECT lblL = { 4, legendY, 90, legendY + DOCK_LEGEND_H };
            RECT lblR = { rc.right - 90, legendY, rc.right - 4, legendY + DOCK_LEGEND_H };
            DrawText(mem, L"低频", -1, &lblL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawText(mem, L"高频", -1, &lblR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            g_maybeDrag = true;
            g_dragging = false;
            GetCursorPos(&g_dragStart);
            g_dragBarOrig.x = g_dockX;
            g_dragBarOrig.y = g_dockY;
            SetCapture(hwnd);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!g_maybeDrag) return 0;
            POINT pt;
            GetCursorPos(&pt);
            int dx = pt.x - g_dragStart.x;
            int dy = pt.y - g_dragStart.y;
            int thx = GetSystemMetrics(SM_CXDRAG);
            int thy = GetSystemMetrics(SM_CYDRAG);
            if (!g_dragging) {
                if (abs(dx) <= thx && abs(dy) <= thy) return 0;
                g_dragging = true;
            }
            g_dockX = g_dragBarOrig.x + dx;
            g_dockY = g_dragBarOrig.y + dy;
            ClampDockPos();
            DockLayout(true);   // 整体平移
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            g_maybeDrag = false;
            g_dragging = false;
            return 0;
        }

        case WM_RBUTTONUP: {
            ShowDockMenu(hwnd);
            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDM_TRAY_TOGGLE_DOCK:
                    ToggleDock();
                    break;
                case IDM_DOCK_EJECT_ALL:
                    for (auto& kv : g_stickyNotes) {
                        UndockNote(*kv.second, true);
                    }
                    break;
            }
            return 0;
        }

        case WM_CLOSE: {
            return 0;   // 不允许单独关闭，只能通过菜单退出聚合模式
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ─── 注册并创建聚合条窗口 ───
static void CreateDockBar() {
    if (g_hDock) return;
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DockWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.hbrBackground = CreateSolidBrush(DOCK_BAR_BG);
    wc.lpszClassName = L"WinTopDockBar";
    RegisterClassEx(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    g_dockX = (sw - 400) / 2;
    g_dockY = 60;

    g_hDock = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"WinTopDockBar", L"WinTop 聚合栏",
        WS_POPUP,
        g_dockX, g_dockY, 400, 120,
        nullptr, nullptr, g_hInst, nullptr
    );
    if (g_hDock) ShowWindow(g_hDock, SW_SHOW);
}
