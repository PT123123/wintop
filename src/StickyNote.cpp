#include "wintop.h"

#define STICKY_WIDTH  320
#define STICKY_HEIGHT 240
#define STICKY_PADDING 8
#define FILTER_ALPHA    70

#define COLOR_NORMAL    RGB(40, 40, 40)
#define COLOR_STALE     RGB(180, 60, 60)
#define COLOR_PAUSED    RGB(60, 90, 180)
#define COLOR_BORDER    RGB(80, 80, 80)

#define FILTER_RED      RGB(255, 70, 70)
#define FILTER_GREEN    RGB(70, 255, 90)
#define FILTER_BLUE     RGB(80, 90, 255)
#define FILTER_WARM     RGB(255, 200, 110)
#define FILTER_GRAY     RGB(120, 120, 120)

static ATOM g_stickyClassAtom = 0;
static ATOM g_filterClassAtom = 0;

// ─── 预览区域（客户端减去内边距） ───
static RECT PreviewRect(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT reg = { STICKY_PADDING, STICKY_PADDING,
                 rc.right - STICKY_PADDING, rc.bottom - STICKY_PADDING };
    return reg;
}

// ─── 滤镜覆盖子窗口过程：纯色半透明 ───
static LRESULT CALLBACK FilterWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            COLORREF color = (COLORREF)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            HBRUSH br = CreateSolidBrush(color);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void RegisterFilterClass() {
    if (g_filterClassAtom) return;
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = FilterWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WinTopFilter";
    g_filterClassAtom = RegisterClassEx(&wc);
}

// ─── 注册便签窗口类 ───
static void RegisterStickyNoteClass() {
    if (g_stickyClassAtom) return;
    
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StickyNoteWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COLOR_NORMAL);
    wc.lpszClassName = L"WinTopStickyNote";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    
    g_stickyClassAtom = RegisterClassEx(&wc);
}

// ─── 创建便签窗口 ───
HWND CreateStickyNoteWindow(HWND hParent) {
    RegisterStickyNoteClass();
    RegisterFilterClass();
    
    int x = 100 + (rand() % 400);
    int y = 100 + (rand() % 300);
    
    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"WinTopStickyNote",
        L"WinTop Preview",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX,
        x, y, STICKY_WIDTH, STICKY_HEIGHT,
        hParent, nullptr, g_hInst, nullptr
    );
    
    if (hwnd) {
        RECT reg = PreviewRect(hwnd);
        
        // 滤镜覆盖子窗口（半透明，位于缩略图之上）
        CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_LAYERED, L"WinTopFilter", L"",
                        WS_CHILD, reg.left, reg.top,
                        reg.right - reg.left, reg.bottom - reg.top,
                        hwnd, nullptr, g_hInst, nullptr);
        
        ShowWindow(hwnd, SW_SHOW);
    }
    
    return hwnd;
}

// ─── 设置刷新定时器（每个便签唯一的刷新驱动源） ───
static void SetRefreshTimer(StickyNote& note) {
    if (note.refreshTimerId) {
        KillTimer(note.hwnd, note.refreshTimerId);
    }
    note.refreshTimerId = SetTimer(note.hwnd, reinterpret_cast<UINT_PTR>(note.hwnd),
                                    note.refreshIntervalSec * 1000, nullptr);
}

// ─── 更新/显隐滤镜子窗口 ───
static void UpdateFilterWindow(StickyNote& note) {
    if (!note.filterWnd) return;
    if (note.hasFilter) {
        SetWindowLongPtr(note.filterWnd, GWLP_USERDATA, (LONG_PTR)note.filterColor);
        SetLayeredWindowAttributes(note.filterWnd, 0, FILTER_ALPHA, LWA_ALPHA);
        ShowWindow(note.filterWnd, SW_SHOW);
        InvalidateRect(note.filterWnd, nullptr, TRUE);
    } else {
        ShowWindow(note.filterWnd, SW_HIDE);
    }
}

// ─── 更新 DWM 缩略图（随状态显隐：暂停/停滞时隐藏） ───
void UpdateStickyNoteThumbnail(StickyNote& note) {
    if (!note.thumbnailId) return;
    
    RECT reg = PreviewRect(note.hwnd);
    
    DWM_THUMBNAIL_PROPERTIES props = {};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    props.rcDestination = reg;
    props.fVisible = (note.thumbnailId != nullptr) && !note.paused && !note.isStale;
    props.opacity = 255;
    props.fSourceClientAreaOnly = TRUE;
    
    DwmUpdateThumbnailProperties(note.thumbnailId, &props);
    
    // 滤镜窗口覆盖在缩略图区域之上
    if (note.filterWnd) {
        SetWindowPos(note.filterWnd, nullptr, reg.left, reg.top,
                     reg.right - reg.left, reg.bottom - reg.top,
                     SWP_NOZORDER);
        UpdateFilterWindow(note);
    }
}

// ─── 绑定目标窗口 ───
void BindStickyNoteToWindow(StickyNote& note, HWND targetHwnd) {
    note.targetHwnd = targetHwnd;
    
    int len = GetWindowTextLength(targetHwnd) + 1;
    std::wstring title(len, L'\0');
    GetWindowText(targetHwnd, &title[0], len);
    note.targetTitle = title;
    
    SetWindowText(note.hwnd, (L"[预览] " + note.targetTitle).c_str());
    
    // 取得子窗口
    note.filterWnd = FindWindowEx(note.hwnd, nullptr, L"WinTopFilter", nullptr);
    
    // 注册 DWM 缩略图
    if (note.thumbnailId) {
        DwmUnregisterThumbnail(note.thumbnailId);
        note.thumbnailId = nullptr;
    }
    HRESULT hr = DwmRegisterThumbnail(note.hwnd, targetHwnd, &note.thumbnailId);
    if (SUCCEEDED(hr)) {
        UpdateStickyNoteThumbnail(note);
    }
    
    note.isStale = false;
    note.paused = false;
    
    // 启动刷新定时器（驱动停滞检测与最小化刷新）
    SetRefreshTimer(note);
}

// ─── 销毁便签 ───
void DestroyStickyNote(StickyNote& note) {
    if (note.refreshTimerId) {
        KillTimer(note.hwnd, note.refreshTimerId);
        note.refreshTimerId = 0;
    }
    if (note.thumbnailId) {
        DwmUnregisterThumbnail(note.thumbnailId);
        note.thumbnailId = nullptr;
    }
    if (note.hwnd) {
        DestroyWindow(note.hwnd);
        note.hwnd = nullptr;
    }
}

// ─── 设置滤镜 ───
static void SetFilter(StickyNote& note, COLORREF color, bool hasFilter) {
    note.filterColor = color;
    note.hasFilter = hasFilter;
    UpdateStickyNoteThumbnail(note);
}

// ─── 暂停 / 继续 ───
static void TogglePause(StickyNote& note) {
    note.paused = !note.paused;
    if (!note.paused) {
        // 继续：重启定时器并立即刷新
        SetRefreshTimer(note);
        UpdateStickyNoteThumbnail(note);
    } else {
        // 暂停：隐藏缩略图，保留其余
        if (note.refreshTimerId) {
            KillTimer(note.hwnd, note.refreshTimerId);
            note.refreshTimerId = 0;
        }
        UpdateStickyNoteThumbnail(note);
    }
    InvalidateRect(note.hwnd, nullptr, TRUE);
}

// ─── 目标窗口最小化时的强制刷新 ───
static void RefreshMinimized(StickyNote& note) {
    // 最小化窗口不渲染新帧，唯一办法是无激活恢复→让 DWM 渲染→再最小化。
    if (!IsIconic(note.targetHwnd)) return;
    
    ShowWindow(note.targetHwnd, SW_SHOWNOACTIVATE);  // 不抢焦点
    // 等待窗口真正恢复并渲染
    for (int i = 0; i < 5; i++) {
        if (!IsIconic(note.targetHwnd)) break;
        Sleep(30);
    }
    if (note.thumbnailId) {
        UpdateStickyNoteThumbnail(note);
    }
    // 重新最小化（不激活）
    ShowWindow(note.targetHwnd, SW_MINIMIZE);
}

// ─── 每次刷新：停滞检测 + 最小化处理 ───
static void OnRefreshTimer(StickyNote& note) {
    if (!note.targetHwnd || note.paused) return;
    
    // 停滞判定：目标窗口是否失去响应（卡死）
    bool wasStale = note.isStale;
    DWORD_PTR res = 0;
    BOOL responded = (BOOL)SendMessageTimeout(note.targetHwnd, WM_NULL, 0, 0,
                                              SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &res);
    note.isStale = (responded == 0);
    
    if (IsIconic(note.targetHwnd)) {
        // 最小化：强制刷新取新帧
        RefreshMinimized(note);
    } else if (!IsWindowVisible(note.targetHwnd)) {
        // 目标被隐藏：隐藏缩略图避免误导
        if (note.thumbnailId) {
            DWM_THUMBNAIL_PROPERTIES p = {};
            p.dwFlags = DWM_TNP_VISIBLE;
            p.fVisible = FALSE;
            DwmUpdateThumbnailProperties(note.thumbnailId, &p);
        }
    } else {
        // 正常：随需要更新缩略图（DWM 自动实时）
        UpdateStickyNoteThumbnail(note);
    }
    
    if (note.isStale != wasStale) {
        UpdateStickyNoteThumbnail(note);
        InvalidateRect(note.hwnd, nullptr, TRUE);
    }
}

// ─── 设置刷新间隔 ───
static void SetRefreshInterval(StickyNote& note, int seconds) {
    if (note.paused) return;
    note.refreshIntervalSec = seconds;
    SetRefreshTimer(note);
    
    wchar_t buf[64];
    wsprintf(buf, L"[预览 %ds] %s", seconds, note.targetTitle.c_str());
    SetWindowText(note.hwnd, buf);
}

// ─── 右键菜单 ───
static void ShowContextMenu(HWND hwnd, StickyNote& note) {
    HMENU hMenu = CreatePopupMenu();
    
    HMENU hRefreshMenu = CreatePopupMenu();
    AppendMenu(hRefreshMenu, MF_STRING | (note.refreshIntervalSec == 1  ? MF_CHECKED : 0), IDM_REFRESH_1S,  L"1 秒");
    AppendMenu(hRefreshMenu, MF_STRING | (note.refreshIntervalSec == 5  ? MF_CHECKED : 0), IDM_REFRESH_5S,  L"5 秒");
    AppendMenu(hRefreshMenu, MF_STRING | (note.refreshIntervalSec == 10 ? MF_CHECKED : 0), IDM_REFRESH_10S, L"10 秒");
    AppendMenu(hRefreshMenu, MF_STRING | (note.refreshIntervalSec == 30 ? MF_CHECKED : 0), IDM_REFRESH_30S, L"30 秒");
    AppendMenu(hRefreshMenu, MF_STRING | (note.refreshIntervalSec == 60 ? MF_CHECKED : 0), IDM_REFRESH_60S, L"60 秒");
    AppendMenu(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hRefreshMenu), L"刷新频率");
    
    HMENU hFilterMenu = CreatePopupMenu();
    AppendMenu(hFilterMenu, MF_STRING | (!note.hasFilter ? MF_CHECKED : 0), IDM_FILTER_NONE,  L"无");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_RED   ? MF_CHECKED : 0), IDM_FILTER_RED,   L"红色");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_GREEN ? MF_CHECKED : 0), IDM_FILTER_GREEN, L"绿色");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_BLUE  ? MF_CHECKED : 0), IDM_FILTER_BLUE,  L"蓝色");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_WARM  ? MF_CHECKED : 0), IDM_FILTER_WARM,  L"暖黄");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_GRAY  ? MF_CHECKED : 0), IDM_FILTER_GRAY,  L"灰度");
    AppendMenu(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hFilterMenu), L"滤镜颜色");
    
    AppendMenu(hMenu, MF_STRING | (note.paused ? MF_CHECKED : 0), IDM_PAUSE_RESUME,
               note.paused ? L"继续预览" : L"暂停预览");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, IDM_CLOSE_STICKY, L"关闭预览");
    
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);
    
    DestroyMenu(hMenu);
    DestroyMenu(hRefreshMenu);
    DestroyMenu(hFilterMenu);
}

// ─── 便签窗口过程 ───
LRESULT CALLBACK StickyNoteWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto it = g_stickyNotes.find(hwnd);
    StickyNote* note = (it != g_stickyNotes.end()) ? it->second.get() : nullptr;
    
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            RECT reg = PreviewRect(hwnd);
            
            // 背景
            HBRUSH bg = CreateSolidBrush(COLOR_NORMAL);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            
            // 边框
            HPEN pen = CreatePen(PS_SOLID, 1, COLOR_BORDER);
            SelectObject(hdc, pen);
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, reg.left, reg.top, reg.right, reg.bottom);
            DeleteObject(pen);
            
            // 停滞 / 暂停状态文字（此时缩略图已隐藏，文字可见）
            if (note && note->isStale && !note->paused) {
                HBRUSH sr = CreateSolidBrush(COLOR_STALE);
                FillRect(hdc, &reg, sr);
                DeleteObject(sr);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                SelectObject(hdc, g_hFont);
                DrawText(hdc, L"[画面停滞]", -1, &reg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else if (note && note->paused) {
                HBRUSH pr = CreateSolidBrush(COLOR_PAUSED);
                FillRect(hdc, &reg, pr);
                DeleteObject(pr);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                SelectObject(hdc, g_hFont);
                DrawText(hdc, L"[已暂停]", -1, &reg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_SIZE: {
            if (note) {
                UpdateStickyNoteThumbnail(*note);
            }
            return 0;
        }
        
        case WM_CONTEXTMENU: {
            if (note) {
                ShowContextMenu(hwnd, *note);
            }
            return 0;
        }
        
        case WM_COMMAND: {
            if (!note) break;
            switch (LOWORD(wParam)) {
                case IDM_REFRESH_1S:  SetRefreshInterval(*note, 1);  break;
                case IDM_REFRESH_5S:  SetRefreshInterval(*note, 5);  break;
                case IDM_REFRESH_10S: SetRefreshInterval(*note, 10); break;
                case IDM_REFRESH_30S: SetRefreshInterval(*note, 30); break;
                case IDM_REFRESH_60S: SetRefreshInterval(*note, 60); break;
                case IDM_FILTER_NONE:  SetFilter(*note, 0, false);              break;
                case IDM_FILTER_RED:   SetFilter(*note, FILTER_RED, true);      break;
                case IDM_FILTER_GREEN: SetFilter(*note, FILTER_GREEN, true);    break;
                case IDM_FILTER_BLUE:  SetFilter(*note, FILTER_BLUE, true);     break;
                case IDM_FILTER_WARM:  SetFilter(*note, FILTER_WARM, true);     break;
                case IDM_FILTER_GRAY:  SetFilter(*note, FILTER_GRAY, true);     break;
                case IDM_PAUSE_RESUME:
                    TogglePause(*note);
                    break;
                case IDM_CLOSE_STICKY:
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                    break;
            }
            return 0;
        }
        
        case WM_TIMER: {
            if (note) {
                OnRefreshTimer(*note);
            }
            return 0;
        }
        
        case WM_CLOSE: {
            if (note) {
                HWND self = note->hwnd;
                if (note->refreshTimerId) {
                    KillTimer(self, note->refreshTimerId);
                    note->refreshTimerId = 0;
                }
                if (note->thumbnailId) {
                    DwmUnregisterThumbnail(note->thumbnailId);
                    note->thumbnailId = nullptr;
                }
                note->hwnd = nullptr;
                g_stickyNotes.erase(self);
                DestroyWindow(self);
            }
            return 0;
        }
        
        case WM_DESTROY: {
            return 0;
        }
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}