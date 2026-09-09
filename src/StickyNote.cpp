#include "wintop.h"
#include <cmath>
#include <cstring>
#pragma comment(lib, "msimg32.lib")

// 工程定义了 NOMINMAX，这里补上本地 min/max（仅本文件使用）
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

// 前置声明（定义在文件后部）
static void FreeHueWarningBuf(StickyNote& note);

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
    wc.hIcon = LoadAppIcon(32);
    wc.hIconSm = LoadAppIcon(16);
    
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
    FreeHueWarningBuf(note);
    if (note.hwnd) {
        DestroyWindow(note.hwnd);
        note.hwnd = nullptr;
    }
}

// ─── 设置滤镜 ───
static void FreeHueWarningBuf(StickyNote& note) {
    if (note.diffBuf) { delete[] note.diffBuf; note.diffBuf = nullptr; }
    note.diffW = note.diffH = 0;
}

static void SetFilter(StickyNote& note, COLORREF color, bool hasFilter) {
    note.filterColor = color;
    note.hasFilter = hasFilter;
    note.useHueWarning = false;
    FreeHueWarningBuf(note);
    InvalidateRect(note.hwnd, nullptr, TRUE);
}

// 启用/停用"色相渐变警戒色标"
static void SetHueWarning(StickyNote& note, bool on) {
    note.useHueWarning = on;
    note.hasFilter = false;
    note.warningLevel = 0.0f;
    if (!on) { FreeHueWarningBuf(note); }
    InvalidateRect(note.hwnd, nullptr, TRUE);
}

// ─── 色相渐变警戒色标：由画面变化程度计算警戒颜色 ───

// HSB -> RGB
static COLORREF HSBtoRGB(double h, double s, double v) {
    double r = 0, g = 0, b = 0;
    h = std::fmod(h, 360.0); if (h < 0) h += 360.0;
    int i = (int)(h / 60) % 6;
    double f = h / 60 - i;
    double p = v * (1 - s);
    double q = v * (1 - f * s);
    double t = v * (1 - (1 - f) * s);
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return RGB((BYTE)(r * 255 + 0.5), (BYTE)(g * 255 + 0.5), (BYTE)(b * 255 + 0.5));
}

// 警戒级别 level(0..1) -> 色相：安全为绿(120°)，随级别升高向红(0°)渐变，最高警戒为红
static COLORREF HueColorFromLevel(float level) {
    double h = (1.0 - level) * 120.0;   // 0 -> 绿，1 -> 红
    return HSBtoRGB(h, 0.82, 0.92);
}

// 把当前帧缩成 48 宽的单通道亮度缩略帧，与上一帧对比变化量并更新警戒级别
static const int kDiffWidth = 48;

static void UpdateHueWarning(StickyNote& note) {
    if (!note.useHueWarning || !note.frame) return;
    
    BITMAP bm;
    GetObject(note.frame, sizeof(bm), &bm);
    if (bm.bmWidth <= 0 || bm.bmHeight <= 0) return;
    int DH = (int)((long long)bm.bmHeight * kDiffWidth / bm.bmWidth);
    if (DH < 1) DH = 1;
    int n = kDiffWidth * DH;
    int stride = kDiffWidth * 3;
    
    // 生成该帧的 24bpp 顶层 DIB 缩略图（读像素用）
    std::vector<BYTE> cur((size_t)n);
    std::vector<BYTE> rgb((size_t)stride * DH);
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = kDiffWidth;
        bi.bmiHeader.biHeight = -DH;              // 顶层 DIB
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 24;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dstDC = CreateCompatibleDC(nullptr);
        HBITMAP dib = CreateDIBSection(dstDC, &bi, DIB_RGB_COLORS, (void**)rgb.data(), nullptr, 0);
        HBITMAP oldDib = (HBITMAP)SelectObject(dstDC, dib);
        HDC srcDC = CreateCompatibleDC(dstDC);
        HBITMAP oldSrc = (HBITMAP)SelectObject(srcDC, note.frame);
        SetStretchBltMode(dstDC, COLORONCOLOR);
        StretchBlt(dstDC, 0, 0, kDiffWidth, DH, srcDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        GdiFlush();
        SelectObject(srcDC, oldSrc); DeleteDC(srcDC);
        SelectObject(dstDC, oldDib); DeleteObject(dib); DeleteDC(dstDC);
        for (int y = 0; y < DH; ++y) {
            const BYTE* row = rgb.data() + (size_t)y * stride;
            BYTE* out = cur.data() + (size_t)y * kDiffWidth;
            for (int x = 0; x < kDiffWidth; ++x) {
                const BYTE* p = row + x * 3;      // 24bpp 内存序为 B,G,R
                out[x] = (BYTE)((p[0] * 11 + p[1] * 59 + p[2] * 30) / 100);
            }
        }
    }
    
    // 与上一帧缩略图对比，计算变化量 0..1
    double diff = 1.0;   // 首帧默认视为有变化（安全）
    if (note.diffBuf && note.diffW == kDiffWidth && note.diffH == DH) {
        long long sum = 0;
        for (int i = 0; i < n; ++i) {
            int d = cur[i] - note.diffBuf[i];
            if (d < 0) d = -d;
            sum += d;
        }
        diff = sum / (double)n / 255.0;
    }
    
    // 需要先扩容/建缓存
    if (!note.diffBuf || note.diffW != kDiffWidth || note.diffH != DH) {
        if (note.diffBuf) delete[] note.diffBuf;
        note.diffBuf = new BYTE[n];
        note.diffW = kDiffWidth;
        note.diffH = DH;
    }
    memcpy(note.diffBuf, cur.data(), (size_t)n);
    
    // 指数平滑逼近目标：目标 = 1 - 变化量。g_warningSamples 越大，到达最高警戒越慢
    double target = 1.0 - diff;
    int k = (g_warningSamples > 0) ? g_warningSamples : 8;
    double alpha = 1.0 - std::exp(-1.0 / k);
    note.warningLevel = (float)(note.warningLevel + (target - note.warningLevel) * alpha);
    if (note.warningLevel < 0.0f) note.warningLevel = 0.0f;
    if (note.warningLevel > 1.0f) note.warningLevel = 1.0f;
}

// ─── 裁剪相关 ───

// 客户端坐标 → 帧像素坐标（与显示时的伸缩映射一致，保证"框到哪显示哪"）
static POINT ClientToFrame(StickyNote& note, POINT pt) {
    RECT reg = PreviewRect(note.hwnd);
    POINT out = {0, 0};
    if (!note.frame) return out;
    BITMAP bm;
    GetObject(note.frame, sizeof(bm), &bm);
    int rw = reg.right - reg.left;
    int rh = reg.bottom - reg.top;
    if (rw <= 0 || rh <= 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) return out;
    out.x = (LONG)((pt.x - reg.left) * (double)bm.bmWidth / rw);
    out.y = (LONG)((pt.y - reg.top) * (double)bm.bmHeight / rh);
    if (out.x < 0) out.x = 0;
    if (out.y < 0) out.y = 0;
    if (out.x > bm.bmWidth) out.x = bm.bmWidth;
    if (out.y > bm.bmHeight) out.y = bm.bmHeight;
    return out;
}

// 进入/退出裁剪选择模式
static void BeginCrop(StickyNote& note) {
    note.cropping = true;
    note.hasCrop = false;
    note.cropStart = note.cropCur = {0, 0};
    InvalidateRect(note.hwnd, nullptr, TRUE);
}

static void CancelCrop(StickyNote& note) {
    note.cropping = false;
    InvalidateRect(note.hwnd, nullptr, TRUE);
}

static void ResetCrop(StickyNote& note) {
    note.cropping = false;
    note.hasCrop = false;
    InvalidateRect(note.hwnd, nullptr, TRUE);
}

// 结束框选：把客户端选区映射成帧像素坐标的裁剪区
static void EndCrop(StickyNote& note) {
    note.cropping = false;
    
    RECT reg = PreviewRect(note.hwnd);
    LONG l = min(note.cropStart.x, note.cropCur.x);
    LONG t = min(note.cropStart.y, note.cropCur.y);
    LONG r = max(note.cropStart.x, note.cropCur.x);
    LONG b = max(note.cropStart.y, note.cropCur.y);
    
    l = max(l, reg.left);  t = max(t, reg.top);
    r = min(r, reg.right); b = min(b, reg.bottom);
    
    if (!note.frame || (r - l < 8) || (b - t < 8)) {
        InvalidateRect(note.hwnd, nullptr, TRUE);
        return;  // 选区太小，视为取消
    }
    
    POINT a = ClientToFrame(note, {l, t});
    POINT d = ClientToFrame(note, {r, b});
    note.cropRect.left = min(a.x, d.x);
    note.cropRect.top = min(a.y, d.y);
    note.cropRect.right = max(a.x, d.x);
    note.cropRect.bottom = max(a.y, d.y);
    note.hasCrop = true;
    
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
            if (note.useHueWarning) UpdateHueWarning(note);   // 采集到位后更新警戒级别
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
    AppendMenu(hFilterMenu, MF_STRING | (!note.hasFilter && !note.useHueWarning ? MF_CHECKED : 0), IDM_FILTER_NONE,  L"无");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_RED   ? MF_CHECKED : 0), IDM_FILTER_RED,   L"红色");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_GREEN ? MF_CHECKED : 0), IDM_FILTER_GREEN, L"绿色");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_BLUE  ? MF_CHECKED : 0), IDM_FILTER_BLUE,  L"蓝色");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_WARM  ? MF_CHECKED : 0), IDM_FILTER_WARM,  L"暖黄");
    AppendMenu(hFilterMenu, MF_STRING | (note.hasFilter && note.filterColor == FILTER_GRAY  ? MF_CHECKED : 0), IDM_FILTER_GRAY,  L"灰度");
    AppendMenu(hFilterMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hFilterMenu, MF_STRING | (note.useHueWarning ? MF_CHECKED : 0), IDM_FILTER_HUE_WARNING, L"色相渐变警戒色标");
    AppendMenu(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hFilterMenu), L"滤镜（颜色 / 警戒色标）");
    
    AppendMenu(hMenu, MF_STRING | (note.paused ? MF_CHECKED : 0), IDM_PAUSE_RESUME,
               note.paused ? L"继续预览" : L"暂停预览");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, IDM_CROP, note.hasCrop ? L"重新裁剪" : L"裁剪");
    if (note.hasCrop) {
        AppendMenu(hMenu, MF_STRING, IDM_CROP_RESET, L"恢复（取消裁剪）");
    }
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
            
            // 绘制当前帧（裁剪时只显示裁剪区域）
            if (note && note->frame) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP old = (HBITMAP)SelectObject(hdcMem, note->frame);
                BITMAP bm;
                GetObject(note->frame, sizeof(bm), &bm);
                SetStretchBltMode(hdc, COLORONCOLOR);
                if (note->hasCrop) {
                    int sw = note->cropRect.right - note->cropRect.left;
                    int sh = note->cropRect.bottom - note->cropRect.top;
                    if (sw > 0 && sh > 0) {
                        StretchBlt(hdc, reg.left, reg.top,
                                   reg.right - reg.left, reg.bottom - reg.top,
                                   hdcMem, note->cropRect.left, note->cropRect.top,
                                   sw, sh, SRCCOPY);
                    }
                } else {
                    StretchBlt(hdc, reg.left, reg.top,
                               reg.right - reg.left, reg.bottom - reg.top,
                               hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                }
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
            if (note && (note->hasFilter || note->useHueWarning)) {
                int fw = reg.right - reg.left;
                int fh = reg.bottom - reg.top;
                if (fw > 0 && fh > 0) {
                    // 取覆盖色：普通滤镜用固定色；警戒色标用当前警戒级别映射的颜色
                    COLORREF color = note->hasFilter ? note->filterColor
                                                     : HueColorFromLevel(note->warningLevel);
                    BYTE alpha = note->useHueWarning ? 120 : FILTER_ALPHA;  // 警戒色标更明显些
                    HDC mem = CreateCompatibleDC(hdc);
                    HBITMAP cb = CreateCompatibleBitmap(hdc, fw, fh);
                    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, cb);
                    HBRUSH fb = CreateSolidBrush(color);
                    RECT frc = { 0, 0, fw, fh };
                    FillRect(mem, &frc, fb);
                    DeleteObject(fb);
                    BLENDFUNCTION bf = {};
                    bf.BlendOp = AC_SRC_OVER;
                    bf.SourceConstantAlpha = alpha;
                    AlphaBlend(hdc, reg.left, reg.top, fw, fh, mem, 0, 0, fw, fh, bf);
                    SelectObject(mem, oldBmp);
                    DeleteObject(cb);
                    DeleteDC(mem);
                }
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
            
            // 裁剪框选：外圈暗化 + 高亮选区 + 提示
            if (note && note->cropping) {
                RECT sel = { min(note->cropStart.x, note->cropCur.x),
                             min(note->cropStart.y, note->cropCur.y),
                             max(note->cropStart.x, note->cropCur.x),
                             max(note->cropStart.y, note->cropCur.y) };
                sel.left = max(sel.left, reg.left);   sel.top = max(sel.top, reg.top);
                sel.right = min(sel.right, reg.right); sel.bottom = min(sel.bottom, reg.bottom);
                
                // 外圈暗化（四块）
                HBRUSH overlay = CreateSolidBrush(RGB(20, 20, 20));
                RECT strips[] = {
                    { reg.left, reg.top, sel.right, sel.top },
                    { reg.left, sel.top, sel.left, sel.bottom },
                    { sel.right, sel.top, reg.right, sel.bottom },
                    { reg.left, sel.bottom, reg.right, reg.bottom }
                };
                for (auto& s : strips) {
                    if (s.right > s.left && s.bottom > s.top) FillRect(hdc, &s, overlay);
                }
                DeleteObject(overlay);
                
                // 选区高亮边框
                HPEN selPen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
                SelectObject(hdc, selPen);
                SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, sel.left, sel.top, sel.right, sel.bottom);
                DeleteObject(selPen);
                
                // 提示
                HFONT oldFont = (HFONT)SelectObject(hdc, g_hFont);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                RECT hint = { reg.left, reg.top, reg.right, reg.top + 26 };
                FillRect(hdc, &hint, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
                DrawText(hdc, L"拖动框选裁剪区域（右键 / Esc 取消）", -1, &hint,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
                case IDM_FILTER_HUE_WARNING:
                    SetHueWarning(*note, !note->useHueWarning);
                    break;
                case IDM_PAUSE_RESUME:
                    TogglePause(*note);
                    break;
                case IDM_CROP:
                    BeginCrop(*note);
                    break;
                case IDM_CROP_RESET:
                    ResetCrop(*note);
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
        
        // 裁剪框选输入
        case WM_LBUTTONDOWN: {
            if (note && note->cropping) {
                note->cropStart = note->cropCur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (note && note->cropping && (GetCapture() == hwnd)) {
                note->cropCur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (note && note->cropping && (GetCapture() == hwnd)) {
                ReleaseCapture();
                EndCrop(*note);
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            // 裁剪选择中右键取消
            if (note && note->cropping) {
                CancelCrop(*note);
                return 0;
            }
            break;  // 否则交给 DefWindowProc / WM_CONTEXTMENU
        }
        case WM_KEYDOWN: {
            if (note && note->cropping && wParam == VK_ESCAPE) {
                CancelCrop(*note);
                return 0;
            }
            break;
        }
        case WM_SETCURSOR: {
            if (note && note->cropping) {
                SetCursor(LoadCursor(nullptr, IDC_CROSS));
                return TRUE;
            }
            break;
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