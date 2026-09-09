#pragma once

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ─── 常量 ───

#define WM_APP_HOTKEY_NEW_STICKY    (WM_APP + 1)
#define WM_APP_HOTKEY_DESTROY_ALL   (WM_APP + 2)
#define WM_APP_TIMER_REFRESH        (WM_APP + 3)
#define WM_APP_TIMER_STALE_CHECK    (WM_APP + 4)
#define WM_APP_TRAY_ICON            (WM_APP + 5)
#define WM_APP_RESHOW_PANEL         (WM_APP + 6)

#define ID_HOTKEY_NEW_STICKY        1
#define ID_HOTKEY_DESTROY_ALL       2
#define ID_HOTKEY_SHOW_PICKER       3

// ─── 托盘菜单 ID ───
#define IDM_TRAY_OPEN_PICKER        2001
#define IDM_TRAY_SHOW_PANEL         2002
#define IDM_TRAY_DESTROY_ALL        2003
#define IDM_TRAY_EXIT               2004

// ─── 右键菜单 ID ───
#define IDM_REFRESH_1S              3001
#define IDM_REFRESH_5S              3002
#define IDM_REFRESH_10S             3003
#define IDM_REFRESH_30S             3004
#define IDM_REFRESH_60S             3005
#define IDM_CLOSE_STICKY            3010
#define IDM_PAUSE_RESUME            3011
#define IDM_FILTER_NONE             3012
#define IDM_FILTER_RED              3013
#define IDM_FILTER_GREEN            3014
#define IDM_FILTER_BLUE             3015
#define IDM_FILTER_WARM             3016
#define IDM_FILTER_GRAY             3017

// ─── 悬浮图标菜单 ID ───
#define IDM_ICON_HIDE               3020

// ─── 托盘图标 ID ───
#define IDI_TRAY_ICON               4001

// ─── 结构体 ───

struct StickyNote {
    HWND hwnd;                          // 便签窗口句柄
    HWND targetHwnd;                    // 目标窗口句柄
    std::wstring targetTitle;           // 目标窗口标题
    
    HTHUMBNAIL thumbnailId;             // DWM 缩略图 ID（实时渲染源窗口）
    HWND filterWnd;                     // 颜色滤镜覆盖子窗口
    HWND tintWnd;                       // 暂停/停滞的半透明遮罩子窗口
    
    // 滤镜
    bool hasFilter;                     // 是否启用颜色滤镜
    COLORREF filterColor;               // 滤镜颜色
    
    // 状态
    bool isStale;                       // 是否停滞（目标窗口失去响应）
    bool paused;                        // 是否暂停（冻结画面）
    int refreshIntervalSec;             // 该便签的刷新间隔（秒）
    UINT_PTR refreshTimerId;            // 该便签的刷新定时器 ID
    
    StickyNote() : hwnd(nullptr), targetHwnd(nullptr),
                   thumbnailId(nullptr), filterWnd(nullptr), tintWnd(nullptr),
                   hasFilter(false), filterColor(0),
                   isStale(false), paused(false), refreshIntervalSec(5),
                   refreshTimerId(0) {}
};

struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    DWORD processId;
};

// ─── 全局变量 ───

extern HINSTANCE g_hInst;
extern HWND g_hMainWnd;
extern std::map<HWND, std::unique_ptr<StickyNote>> g_stickyNotes;
extern int g_refreshIntervalSec;
extern int g_staleThresholdSec;
extern HFONT g_hFont;

// ─── 函数声明 ───

// WindowEnumerator.cpp
std::vector<WindowInfo> EnumerateVisibleWindows();

// StickyNote.cpp
HWND CreateStickyNoteWindow(HWND hParent);
void BindStickyNoteToWindow(StickyNote& note, HWND targetHwnd);
void UpdateStickyNoteThumbnail(StickyNote& note);
void DestroyStickyNote(StickyNote& note);
LRESULT CALLBACK StickyNoteWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// PickerDialog.cpp
void ShowWindowPicker(HWND hOwner);

// ControlPanel.cpp
HWND CreateControlPanel(HWND hOwner);
void ShowControlPanel();
void HideControlPanel();
void DestroyControlPanel();

// Main.cpp
void RegisterHotKeys(HWND hwnd);
void UnregisterHotKeys(HWND hwnd);
void CreateAndBindStickyNote(HWND targetHwnd);
