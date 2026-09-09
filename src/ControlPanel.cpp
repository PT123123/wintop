#include "wintop.h"

// ─── 悬浮图标常量 ───
#define ICON_W          44
#define ICON_H          44
#define ICON_BG         RGB(30, 30, 35)
#define ICON_BORDER     RGB(70, 120, 220)
#define ICON_TEXT       RGB(240, 240, 240)

static HWND g_hPanel = nullptr;
static HFONT g_hIconFont = nullptr;

static void EnsureFonts() {
    if (!g_hIconFont) {
        g_hIconFont = CreateFont(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
}

// ─── 显示右键菜单 ───
static void ShowIconMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, IDM_TRAY_OPEN_PICKER, L"新建预览便签");
    AppendMenu(hMenu, MF_STRING, IDM_TRAY_DESTROY_ALL, L"关闭所有便签");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, IDM_ICON_HIDE, L"隐藏此图标");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, IDM_TRAY_EXIT, L"退出");
    
    POINT pt;
    GetCursorPos(&pt);
    
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

// ─── 拖拽状态 ───
static POINT g_dragStart = {0, 0};
static RECT  g_dragOrigRect = {0, 0, 0, 0};
static bool  g_dragging = false;
static bool  g_maybeClick = false;

// ─── 窗口过程 ───
static LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            EnsureFonts();
            return 0;
        }
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            
            // 双缓冲
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            SelectObject(mem, bmp);
            
            // 背景圆角矩形
            HBRUSH bg = CreateSolidBrush(ICON_BG);
            FillRect(mem, &rc, bg);
            DeleteObject(bg);
            
            // 边框圆角
            HPEN pen = CreatePen(PS_SOLID, 2, ICON_BORDER);
            SelectObject(mem, pen);
            SelectObject(mem, GetStockObject(NULL_BRUSH));
            RoundRect(mem, 1, 1, rc.right - 1, rc.bottom - 1, 12, 12);
            DeleteObject(pen);
            
            // 图标文字
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, ICON_TEXT);
            SelectObject(mem, g_hIconFont);
            DrawText(mem, L"W", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_LBUTTONDOWN: {
            // 记录拖拽起点，尚未判定是点击还是拖动
            g_maybeClick = true;
            g_dragging = false;
            GetCursorPos(&g_dragStart);
            GetWindowRect(hwnd, &g_dragOrigRect);
            SetCapture(hwnd);
            return 0;
        }
        
        case WM_MOUSEMOVE: {
            if (!g_maybeClick) return 0;
            POINT pt;
            GetCursorPos(&pt);
            int dx = pt.x - g_dragStart.x;
            int dy = pt.y - g_dragStart.y;
            int thx = GetSystemMetrics(SM_CXDRAG);
            int thy = GetSystemMetrics(SM_CYDRAG);
            
            if (!g_dragging) {
                if (abs(dx) <= thx && abs(dy) <= thy) return 0;  // 未超阈值，仍可能是点击
                g_dragging = true;  // 超过阈值，进入拖拽
            }
            SetWindowPos(hwnd, nullptr,
                         g_dragOrigRect.left + dx, g_dragOrigRect.top + dy,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER);
            return 0;
        }
        
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (g_maybeClick && !g_dragging) {
                // 视为点击：打开窗口选择器
                ShowWindowPicker(g_hMainWnd);
            }
            g_maybeClick = false;
            g_dragging = false;
            return 0;
        }
        
        case WM_RBUTTONUP: {
            ShowIconMenu(hwnd);
            return 0;
        }
        
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDM_TRAY_OPEN_PICKER:
                    ShowWindowPicker(g_hMainWnd);
                    break;
                case IDM_TRAY_DESTROY_ALL:
                    PostMessage(g_hMainWnd, WM_COMMAND, IDM_TRAY_DESTROY_ALL, 0);
                    break;
                case IDM_ICON_HIDE:
                    ShowWindow(hwnd, SW_HIDE);
                    break;
                case IDM_TRAY_EXIT:
                    PostMessage(g_hMainWnd, WM_CLOSE, 0, 0);
                    break;
            }
            return 0;
        }
        
        case WM_CLOSE: {
            // 只隐藏，不销毁（可从托盘重新显示）
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ─── 注册并创建 ───
static ATOM g_panelClassAtom = 0;

static void RegisterPanelClass() {
    if (g_panelClassAtom) return;
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.hbrBackground = CreateSolidBrush(ICON_BG);
    wc.lpszClassName = L"WinTopControlPanel";
    g_panelClassAtom = RegisterClassEx(&wc);
}

// ─── 公共 API ───
HWND CreateControlPanel(HWND hOwner) {
    RegisterPanelClass();
    
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    // 右下角偏移
    int x = screenW - ICON_W - 60;
    int y = screenH - ICON_H - 80;
    
    g_hPanel = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"WinTopControlPanel", L"WinTop Preview",
        WS_POPUP,
        x, y, ICON_W, ICON_H,
        hOwner, nullptr, g_hInst, nullptr
    );
    
    if (g_hPanel) ShowWindow(g_hPanel, SW_SHOW);
    return g_hPanel;
}

void ShowControlPanel() {
    if (g_hPanel) {
        ShowWindow(g_hPanel, SW_SHOW);
        SetForegroundWindow(g_hPanel);
    }
}

void HideControlPanel() {
    if (g_hPanel) ShowWindow(g_hPanel, SW_HIDE);
}

void DestroyControlPanel() {
    if (g_hPanel) { DestroyWindow(g_hPanel); g_hPanel = nullptr; }
    if (g_hIconFont) { DeleteObject(g_hIconFont); g_hIconFont = nullptr; }
}