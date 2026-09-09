#include "wintop.h"
#include <algorithm>

// ─── 颜色常量 ───
#define BG_COLOR        RGB(30, 30, 35)
#define ITEM_BG         RGB(45, 45, 50)
#define ITEM_HOVER      RGB(60, 60, 70)
#define ITEM_SELECTED   RGB(70, 100, 180)
#define TEXT_COLOR      RGB(240, 240, 240)
#define TITLE_COLOR     RGB(180, 180, 190)
#define BORDER_COLOR    RGB(60, 60, 65)
#define SEARCH_BG       RGB(50, 50, 55)

// ─── 尺寸常量 ───
#define PICKER_W        480
#define PICKER_H        520
#define SEARCH_H        40
#define ITEM_H          48
#define PADDING         16
#define SEARCH_MARGIN   12
#define ITEM_GAP        4

// ─── 全局状态 ───
static HWND g_hPicker = nullptr;
static HWND g_hSearch = nullptr;
static std::vector<WindowInfo> g_allWindows;
static std::vector<size_t> g_filteredIndices;  // 指向 g_allWindows 的索引
static int g_selectedIdx = -1;  // g_filteredIndices 中的索引
static int g_scrollOffset = 0;
static HFONT g_hPickerFont = nullptr;
static HFONT g_hSearchFont = nullptr;
static HFONT g_hTitleFont = nullptr;
static std::wstring g_searchText;

// ─── 子类化搜索框 ───
static WNDPROC g_origSearchProc = nullptr;

static void UpdateFilteredWindows() {
    g_filteredIndices.clear();
    g_selectedIdx = -1;
    g_scrollOffset = 0;

    if (g_searchText.empty()) {
        for (size_t i = 0; i < g_allWindows.size(); i++) {
            g_filteredIndices.push_back(i);
        }
    } else {
        std::wstring lower = g_searchText;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        for (size_t i = 0; i < g_allWindows.size(); i++) {
            std::wstring t = g_allWindows[i].title;
            std::transform(t.begin(), t.end(), t.begin(), ::towlower);
            if (t.find(lower) != std::wstring::npos) {
                g_filteredIndices.push_back(i);
            }
        }
    }

    if (!g_filteredIndices.empty()) {
        g_selectedIdx = 0;
    }
}

static void SelectAndCreateSticky() {
    if (g_selectedIdx < 0 || g_selectedIdx >= static_cast<int>(g_filteredIndices.size())) {
        return;
    }
    HWND target = g_allWindows[g_filteredIndices[g_selectedIdx]].hwnd;
    
    // 先关闭选择器
    if (g_hPicker) {
        DestroyWindow(g_hPicker);
        g_hPicker = nullptr;
    }
    
    CreateAndBindStickyNote(target);
}

static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN: {
            if (wParam == VK_DOWN) {
                if (g_selectedIdx < static_cast<int>(g_filteredIndices.size()) - 1) {
                    g_selectedIdx++;
                    // 确保可见
                    int visibleItems = (PICKER_H - SEARCH_H - SEARCH_MARGIN * 2 - PADDING * 2) / (ITEM_H + ITEM_GAP);
                    if (g_selectedIdx >= g_scrollOffset + visibleItems) {
                        g_scrollOffset = g_selectedIdx - visibleItems + 1;
                    }
                    if (g_hPicker) InvalidateRect(g_hPicker, nullptr, TRUE);
                }
                return 0;
            }
            if (wParam == VK_UP) {
                if (g_selectedIdx > 0) {
                    g_selectedIdx--;
                    if (g_selectedIdx < g_scrollOffset) {
                        g_scrollOffset = g_selectedIdx;
                    }
                    if (g_hPicker) InvalidateRect(g_hPicker, nullptr, TRUE);
                }
                return 0;
            }
            if (wParam == VK_RETURN) {
                SelectAndCreateSticky();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (g_hPicker) {
                    DestroyWindow(g_hPicker);
                    g_hPicker = nullptr;
                }
                return 0;
            }
            break;
        }
        case WM_CHAR: {
            // 让默认处理先完成，然后更新过滤
            LRESULT res = CallWindowProc(g_origSearchProc, hwnd, msg, wParam, lParam);
            
            wchar_t buf[256] = {};
            GetWindowText(hwnd, buf, 256);
            g_searchText = buf;
            UpdateFilteredWindows();
            if (g_hPicker) InvalidateRect(g_hPicker, nullptr, TRUE);
            return res;
        }
    }
    return CallWindowProc(g_origSearchProc, hwnd, msg, wParam, lParam);
}

// ─── 获取列表可见区域 ───
static RECT GetListArea(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int top = PADDING + SEARCH_H + SEARCH_MARGIN;
    return { PADDING, top, rc.right - PADDING, rc.bottom - PADDING };
}

static int GetMaxVisibleItems() {
    RECT area = GetListArea(g_hPicker);
    return (area.bottom - area.top) / (ITEM_H + ITEM_GAP);
}

// ─── 鼠标点击检测 ───
static int HitTestItem(LPARAM lParam) {
    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);
    
    RECT area = GetListArea(g_hPicker);
    if (x < area.left || x > area.right) return -1;
    
    for (int i = 0; i < static_cast<int>(g_filteredIndices.size()); i++) {
        int drawIdx = i - g_scrollOffset;
        if (drawIdx < 0) continue;
        
        int itemTop = area.top + drawIdx * (ITEM_H + ITEM_GAP);
        int itemBottom = itemTop + ITEM_H;
        
        if (y >= itemTop && y < itemBottom) {
            return i;
        }
    }
    return -1;
}

// ─── 选择器窗口过程 ───
static LRESULT CALLBACK PickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // 初始化字体
            if (!g_hPickerFont) {
                g_hPickerFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            }
            if (!g_hSearchFont) {
                g_hSearchFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            }
            if (!g_hTitleFont) {
                g_hTitleFont = CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            }
            
            // 创建搜索框
            g_hSearch = CreateWindowEx(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                PADDING, PADDING, PICKER_W - PADDING * 2, SEARCH_H,
                hwnd, nullptr, g_hInst, nullptr
            );
            
            // 子类化搜索框
            g_origSearchProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtr(g_hSearch, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SearchEditProc))
            );
            
            SendMessage(g_hSearch, WM_SETFONT, reinterpret_cast<WPARAM>(g_hSearchFont), TRUE);
            
            // 枚举窗口
            g_allWindows = EnumerateVisibleWindows();
            UpdateFilteredWindows();
            
            SetFocus(g_hSearch);
            return 0;
        }
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rc;
            GetClientRect(hwnd, &rc);
            
            // 双缓冲
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            SelectObject(hdcMem, hBmp);
            
            // 背景
            HBRUSH hBg = CreateSolidBrush(BG_COLOR);
            FillRect(hdcMem, &rc, hBg);
            DeleteObject(hBg);
            
            // 边框
            HPEN hPen = CreatePen(PS_SOLID, 1, BORDER_COLOR);
            SelectObject(hdcMem, hPen);
            SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
            Rectangle(hdcMem, 0, 0, rc.right, rc.bottom);
            DeleteObject(hPen);
            
            // 搜索框背景
            RECT searchRc = { PADDING - 2, PADDING - 2, rc.right - PADDING + 2, PADDING + SEARCH_H + 2 };
            HBRUSH hSearchBg = CreateSolidBrush(SEARCH_BG);
            FillRect(hdcMem, &searchRc, hSearchBg);
            DeleteObject(hSearchBg);
            
            // 搜索框提示文字（如果为空）
            if (g_searchText.empty()) {
                SetBkMode(hdcMem, TRANSPARENT);
                SetTextColor(hdcMem, RGB(120, 120, 130));
                SelectObject(hdcMem, g_hSearchFont);
                RECT textRc = { PADDING + 8, PADDING, rc.right - PADDING, PADDING + SEARCH_H };
                DrawText(hdcMem, L"搜索窗口...", -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
            }
            
            // 列表区域
            RECT area = GetListArea(hwnd);
            int maxVisible = GetMaxVisibleItems();
            
            // 裁剪到列表区域
            HRGN hClip = CreateRectRgn(area.left, area.top, area.right, area.bottom);
            SelectClipRgn(hdcMem, hClip);
            
            for (int i = g_scrollOffset; i < static_cast<int>(g_filteredIndices.size()) && i < g_scrollOffset + maxVisible; i++) {
                int drawIdx = i - g_scrollOffset;
                int itemTop = area.top + drawIdx * (ITEM_H + ITEM_GAP);
                RECT itemRc = { area.left, itemTop, area.right, itemTop + ITEM_H };
                
                // 背景
                COLORREF bg;
                if (i == g_selectedIdx) {
                    bg = ITEM_SELECTED;
                } else {
                    bg = ITEM_BG;
                }
                
                // 圆角矩形（用 FillRect 模拟）
                HBRUSH hItemBg = CreateSolidBrush(bg);
                FillRect(hdcMem, &itemRc, hItemBg);
                DeleteObject(hItemBg);
                
                // 窗口标题
                SetBkMode(hdcMem, TRANSPARENT);
                SetTextColor(hdcMem, TEXT_COLOR);
                SelectObject(hdcMem, g_hPickerFont);
                
                RECT textRc = { itemRc.left + 12, itemRc.top + 4, itemRc.right - 12, itemRc.bottom - 4 };
                DrawText(hdcMem, g_allWindows[g_filteredIndices[i]].title.c_str(), -1, &textRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                
                // 进程 ID
                wchar_t pidBuf[32];
                wsprintf(pidBuf, L"PID: %lu", g_allWindows[g_filteredIndices[i]].processId);
                SetTextColor(hdcMem, TITLE_COLOR);
                SelectObject(hdcMem, g_hTitleFont);
                RECT pidRc = { itemRc.left + 12, itemRc.top + 22, itemRc.right - 12, itemRc.bottom - 4 };
                DrawText(hdcMem, pidBuf, -1, &pidRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            
            SelectClipRgn(hdcMem, nullptr);
            DeleteObject(hClip);
            
            // 底部状态栏
            wchar_t statusBuf[64];
            wsprintf(statusBuf, L"共 %zu 个窗口  |  上/下选择  Enter确认  Esc取消", g_filteredIndices.size());
            SetTextColor(hdcMem, TITLE_COLOR);
            SelectObject(hdcMem, g_hTitleFont);
            RECT statusRc = { PADDING, rc.bottom - 24, rc.right - PADDING, rc.bottom - 4 };
            DrawText(hdcMem, statusBuf, -1, &statusRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            
            // 输出
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
            
            DeleteObject(hBmp);
            DeleteDC(hdcMem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0 && g_scrollOffset > 0) {
                g_scrollOffset--;
            } else if (delta < 0) {
                int maxVisible = GetMaxVisibleItems();
                int maxScroll = static_cast<int>(g_filteredIndices.size()) - maxVisible;
                if (maxScroll < 0) maxScroll = 0;
                if (g_scrollOffset < maxScroll) {
                    g_scrollOffset++;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        
        case WM_MOUSEMOVE: {
            int hit = HitTestItem(lParam);
            if (hit >= 0) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
            }
            return 0;
        }
        
        case WM_LBUTTONDOWN: {
            int hit = HitTestItem(lParam);
            if (hit >= 0) {
                g_selectedIdx = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        
        case WM_LBUTTONDBLCLK: {
            int hit = HitTestItem(lParam);
            if (hit >= 0) {
                g_selectedIdx = hit;
                SelectAndCreateSticky();
            }
            return 0;
        }
        
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                // 失去焦点时关闭
                DestroyWindow(hwnd);
                g_hPicker = nullptr;
            }
            return 0;
        }
        
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                g_hPicker = nullptr;
                return 0;
            }
            break;
        }
        
        case WM_CLOSE: {
            DestroyWindow(hwnd);
            g_hPicker = nullptr;
            return 0;
        }
        
        case WM_DESTROY: {
            g_hPicker = nullptr;
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ─── 注册选择器窗口类 ───
static ATOM g_pickerClassAtom = 0;

static void RegisterPickerClass() {
    if (g_pickerClassAtom) return;
    
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = PickerWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(BG_COLOR);
    wc.lpszClassName = L"WinTopPicker";
    
    g_pickerClassAtom = RegisterClassEx(&wc);
}

// ─── 显示窗口选择器 ───
void ShowWindowPicker(HWND hOwner) {
    // 如果已打开，关闭旧的
    if (g_hPicker) {
        DestroyWindow(g_hPicker);
        g_hPicker = nullptr;
        return;
    }
    
    RegisterPickerClass();
    
    // 居中屏幕
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - PICKER_W) / 2;
    int y = (screenH - PICKER_H) / 3;  // 偏上，类似启动器位置
    
    g_hPicker = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"WinTopPicker",
        L"WinTop Preview - 选择窗口",
        WS_POPUP,
        x, y, PICKER_W, PICKER_H,
        hOwner, nullptr, g_hInst, nullptr
    );
    
    if (g_hPicker) {
        ShowWindow(g_hPicker, SW_SHOW);
        SetForegroundWindow(g_hPicker);
    }
}
