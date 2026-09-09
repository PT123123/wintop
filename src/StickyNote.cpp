#include "wintop.h"
#pragma comment(lib, "msimg32.lib")

#define STICKY_WIDTH    360
#define STICKY_HEIGHT   260
#define STICKY_PADDING  8
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

// ─── 预览区域（客户端减去内边距） ───
static RECT PreviewRect(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT reg = { STICKY_PADDING, STICKY_PADDING,
                 rc.right - STICKY_PADDING, rc.bottom - STICKY_PADDING };
    return reg;
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

// ─── 抓取目标窗口当前内容为新位图（全窗口，含边框） ───
static HBITMAP CaptureWindowContent(HWND target) {
    RECT wrc;
    GetWindowRect(target, &wrc);
    int w = wrc.right - wrc.left;
    int h = wrc.bottom - wrc.top;
    if (w <= 0 || h <= 0) return nullptr;
    
    HDC hdcScreen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(hdcScreen);
    HBITMAP bmp = CreateCompatibleBitmap(hdcScreen, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    
    // 优先 PrintWindow：能拿到目标自身内容（含被遮挡/需重绘时）
    BOOL ok = PrintWindow(target, mem, PW_RENDERFULLCONTENT);
    if (!ok) {
        // 回退：屏幕区域抓取
        ok = BitBlt(mem, 0, 0, w, h, hdcScreen, wrc.left, wrc.top, SRCCOPY);
    }
    
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, hdcScreen);
    
    if (!ok) {
        DeleteObject(bmp);
        return nullptr;
    }
    return bmp;
}

// ─── 绑定目标窗口 ───
void BindStickyNoteToWindow(StickyNote& note, HWND targetHwnd) {
    note.targetHwnd = targetHwnd;
    
    int len = GetWindowTextLength(targetHwnd) + 1;
    std::wstring title(len, L'\0');
    GetWindowText(targetHwnd, &title[0], len);
    note.targetTitle = title;
    
    SetWindowText(note.hwnd, (L"[预览] " + note.targetTitle).c_str());
    
    note.filterWnd = nullptr;
    note.isStale = false;
    note.paused = false;
    
    // 立即抓首帧
    HBITMAP bmp = CaptureWindowContent(targetHwnd);
    if (bmp) {
        if (note.frame) DeleteObject(note.frame);
        note.frame = bmp;
    }
    
    InvalidateRect(note.hwnd, nullptr, TRUE);
    
    // 启动刷新定时器
    SetRefreshTimer(note);
}

// ─── 销毁便签 ───
void DestroyStickyNote(StickyNote& note) {
    if (note.refreshTimerId) {
        KillTimer(note.hwnd, note.refreshTimerId);
        note.refreshTimerId = 0;
    }
    if (note.frame) {
        DeleteObject(note.frame);
        note.frame = nullptr;
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
    InvalidateRect(note.hwnd, nullptr, TRUE);
}

// ─── 暂停 / 继续 ───
static void TogglePause(StickyNote& note) {
    note.paused = !note.paused;
    if (note.paused) {
        // 暂停：停掉刷新定时器，画面冻结在上一帧
        if (note.refreshTimerId) {
            KillTimer(note.hwnd, note.refreshTimerId);
            note.refreshTimerId = 0;
        }
    } else {
        SetRefreshTimer(note);
        InvalidateRect(note.hwnd, nullptr, TRUE);
    }
}

// ─── 每次刷新：抓一帧新画面（对最小化窗口先恢复再抓再最小化） ───
static void RefreshFrame(StickyNote& note) {
    if (!note.targetHwnd || note.paused) return;
    
    bool wasMinimized = IsIconic(note.targetHwnd);
    bool visible = IsWindowVisible(note.targetHwnd);
    
    if (wasMinimized) {
        // 最小化窗口不渲染：先无激活恢复，等它真正绘制（等待时长可在设置中调节），再抓帧
        ShowWindow(note.targetHwnd, SW_SHOWNOACTIVATE);
        DWORD start = GetTickCount();
        while (GetTickCount() - start < (DWORD)g_minimizedWaitMs) {
            Sleep(15);
        }
        visible = IsWindowVisible(note.targetHwnd);
    }
    
    if (!visible && !wasMinimized) {
        // 目标被隐藏（非最小化）：显示空白
        if (note.frame) { DeleteObject(note.frame); note.frame = nullptr; }
        InvalidateRect(note.hwnd, nullptr, TRUE);
    } else {
        HBITMAP bmp = CaptureWindowContent(note.targetHwnd);
        if (bmp) {
            if (note.frame) DeleteObject(note.frame);
            note.frame = bmp;
            InvalidateRect(note.hwnd, nullptr, TRUE);
        }
    }
    
    if (wasMinimized) {
        // 抓完再最小化回去
        ShowWindow(note.targetHwnd, SW_MINIMIZE);
    }
}

// ─── 停滞检测 + 刷新 ───
static void OnRefreshTimer(StickyNote& note) {
    if (!note.targetHwnd || note.paused) return;
    
    bool wasStale = note.isStale;
    DWORD_PTR res = 0;
    BOOL responded = (BOOL)SendMessageTimeout(note.targetHwnd, WM_NULL, 0, 0,
                                              SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &res);
    note.isStale = (responded == 0);
    
    RefreshFrame(note);
    
    if (note.isStale != wasStale) {
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
    AppendMenu(hMenu, MF_STRING, IDM_TRAY_RESTART_ADMIN, L"以管理员模式重启");
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
            
            HBRUSH bg = CreateSolidBrush(COLOR_NORMAL);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            
            // 绘制当前帧
            if (note && note->frame) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP old = (HBITMAP)SelectObject(hdcMem, note->frame);
                BITMAP bm;
                GetObject(note->frame, sizeof(bm), &bm);
                SetStretchBltMode(hdc, COLORONCOLOR);
                StretchBlt(hdc, reg.left, reg.top,
                           reg.right - reg.left, reg.bottom - reg.top,
                           hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                SelectObject(hdcMem, old);
                DeleteDC(hdcMem);
            }
            
            // 边框
            HPEN pen = CreatePen(PS_SOLID, 1, COLOR_BORDER);
            SelectObject(hdc, pen);
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, reg.left, reg.top, reg.right, reg.bottom);
            DeleteObject(pen);
            
            // 颜色滤镜（半透明覆盖在画面上）
            if (note && note->hasFilter) {
                int fw = reg.right - reg.left;
                int fh = reg.bottom - reg.top;
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP cb = CreateCompatibleBitmap(hdc, fw, fh);
                HBITMAP oldBmp = (HBITMAP)SelectObject(mem, cb);
                HBRUSH fb = CreateSolidBrush(note->filterColor);
                RECT frc = { 0, 0, fw, fh };
                FillRect(mem, &frc, fb);
                DeleteObject(fb);
                BLENDFUNCTION bf = {};
                bf.BlendOp = AC_SRC_OVER;
                bf.SourceConstantAlpha = FILTER_ALPHA;
                AlphaBlend(hdc, reg.left, reg.top, fw, fh, mem, 0, 0, fw, fh, bf);
                SelectObject(mem, oldBmp);
                DeleteObject(cb);
                DeleteDC(mem);
            }
            
            // 状态横幅（停滞 / 暂停）
            if (note) {
                HFONT oldFont = (HFONT)SelectObject(hdc, g_hFont);
                SetBkMode(hdc, TRANSPARENT);
                RECT banner = { reg.left, reg.top, reg.right, reg.top + 26 };
                if (note->isStale && !note->paused) {
                    HBRUSH sr = CreateSolidBrush(COLOR_STALE);
                    FillRect(hdc, &banner, sr);
                    DeleteObject(sr);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    DrawText(hdc, L"[画面停滞]", -1, &banner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else if (note->paused) {
                    HBRUSH pr = CreateSolidBrush(COLOR_PAUSED);
                    FillRect(hdc, &banner, pr);
                    DeleteObject(pr);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    DrawText(hdc, L"[已暂停]", -1, &banner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                SelectObject(hdc, oldFont);
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_SIZE: {
            InvalidateRect(hwnd, nullptr, TRUE);
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
                case IDM_TRAY_RESTART_ADMIN:
                    RestartAsAdmin(g_hMainWnd);
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
                if (note->frame) {
                    DeleteObject(note->frame);
                    note->frame = nullptr;
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