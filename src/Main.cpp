#include "wintop.h"

// ─── 全局变量定义 ───

HINSTANCE g_hInst = nullptr;
HWND g_hMainWnd = nullptr;
std::map<HWND, std::unique_ptr<StickyNote>> g_stickyNotes;
int g_refreshIntervalSec = 5;
int g_staleThresholdSec = 10;
int g_minimizedWaitMs = 200;
HFONT g_hFont = nullptr;

// ─── 设置持久化（注册表 HKCU\Software\WinTopPreview） ───

static void LoadSettings() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\WinTopPreview", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD data = 0;
        DWORD size = sizeof(data);
        if (RegQueryValueExW(key, L"MinimizedWaitMs", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&data), &size) == ERROR_SUCCESS) {
            g_minimizedWaitMs = (int)data;
            if (g_minimizedWaitMs < 30) g_minimizedWaitMs = 30;
        }
        RegCloseKey(key);
    }
}

static void SaveSettings() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\WinTopPreview", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        DWORD data = (DWORD)g_minimizedWaitMs;
        RegSetValueExW(key, L"MinimizedWaitMs", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&data), sizeof(data));
        RegCloseKey(key);
    }
}

static int WaitMsFromId(UINT id) {
    switch (id) {
        case IDM_WAIT_50:  return 50;
        case IDM_WAIT_100: return 100;
        case IDM_WAIT_200: return 200;
        case IDM_WAIT_300: return 300;
        case IDM_WAIT_500: return 500;
        case IDM_WAIT_800: return 800;
        case IDM_WAIT_1000: return 1000;
    }
    return g_minimizedWaitMs;
}

// 构建“设置”子菜单
HMENU BuildSettingsMenu() {
    HMENU hSet = CreatePopupMenu();
    HMENU hWait = CreatePopupMenu();
    struct { UINT id; int ms; } vals[] = {
        { IDM_WAIT_50, 50 }, { IDM_WAIT_100, 100 }, { IDM_WAIT_200, 200 },
        { IDM_WAIT_300, 300 }, { IDM_WAIT_500, 500 }, { IDM_WAIT_800, 800 },
        { IDM_WAIT_1000, 1000 }
    };
    wchar_t buf[32];
    for (auto& v : vals) {
        wsprintf(buf, L"%d ms", v.ms);
        AppendMenu(hWait, MF_STRING | (g_minimizedWaitMs == v.ms ? MF_CHECKED : 0), v.id, buf);
    }
    AppendMenu(hSet, MF_POPUP, reinterpret_cast<UINT_PTR>(hWait), L"最小化恢复抓帧等待时长");
    return hSet;
}

// ─── 注册全局热键 ───

void RegisterHotKeys(HWND hwnd) {
    // Ctrl+Alt+N: 打开窗口选择器
    RegisterHotKey(hwnd, ID_HOTKEY_SHOW_PICKER, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'N');
    
    // Ctrl+Alt+X: 销毁所有便签
    RegisterHotKey(hwnd, ID_HOTKEY_DESTROY_ALL, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'X');
}

void UnregisterHotKeys(HWND hwnd) {
    UnregisterHotKey(hwnd, ID_HOTKEY_SHOW_PICKER);
    UnregisterHotKey(hwnd, ID_HOTKEY_DESTROY_ALL);
}

// ─── 创建新便签并绑定窗口 ───

void CreateAndBindStickyNote(HWND targetHwnd) {
    HWND stickyHwnd = CreateStickyNoteWindow(g_hMainWnd);
    if (!stickyHwnd) return;
    
    auto note = std::make_unique<StickyNote>();
    note->hwnd = stickyHwnd;
    
    if (targetHwnd) {
        BindStickyNoteToWindow(*note, targetHwnd);
    }
    
    g_stickyNotes[stickyHwnd] = std::move(note);
}

// ─── 销毁所有便签 ───

static void DestroyAllStickyNotes() {
    for (auto& pair : g_stickyNotes) {
        DestroyStickyNote(*pair.second);
    }
    g_stickyNotes.clear();
}

// ─── 以管理员模式重启 ───

void RestartAsAdmin(HWND hwnd) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileName(nullptr, path, MAX_PATH)) return;
    
    SHELLEXECUTEINFO sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";   // 触发 UAC 提权
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;
    
    if (ShellExecuteEx(&sei)) {
        // 管理员实例已启动，退出当前实例
        PostMessage(hwnd, WM_COMMAND, IDM_TRAY_EXIT, 0);
    } else {
        MessageBox(nullptr, L"无法以管理员身份重新启动（可能已取消）。",
                   L"WinTop Preview", MB_OK | MB_ICONERROR);
    }
}

// ─── 主窗口过程 ───

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_HOTKEY: {
            if (wParam == ID_HOTKEY_SHOW_PICKER) {
                ShowWindowPicker(hwnd);
                return 0;
            } else if (wParam == ID_HOTKEY_DESTROY_ALL) {
                DestroyAllStickyNotes();
                return 0;
            }
            break;
        }
        
        case WM_APP_TRAY_ICON: {
            if (LOWORD(lParam) == WM_RBUTTONUP) {
                HMENU hMenu = CreatePopupMenu();
                AppendMenu(hMenu, MF_STRING, IDM_TRAY_OPEN_PICKER, L"新建预览便签");
                AppendMenu(hMenu, MF_STRING, IDM_TRAY_SHOW_PANEL, L"显示悬浮图标");
                AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenu(hMenu, MF_STRING, IDM_TRAY_DESTROY_ALL, L"关闭所有便签");
                AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenu(hMenu, MF_STRING, IDM_TRAY_RESTART_ADMIN, L"以管理员模式重启");
                AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
                HMENU hSettings = BuildSettingsMenu();
                AppendMenu(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSettings), L"设置");
                AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenu(hMenu, MF_STRING, IDM_TRAY_EXIT, L"退出");
                
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
                PostMessage(hwnd, WM_NULL, 0, 0);
                DestroyMenu(hMenu);
            } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                ShowWindowPicker(hwnd);
            }
            return 0;
        }
        
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDM_TRAY_OPEN_PICKER:
                    ShowWindowPicker(hwnd);
                    break;
                case IDM_TRAY_SHOW_PANEL:
                    ShowControlPanel();
                    break;
                case IDM_TRAY_DESTROY_ALL:
                    DestroyAllStickyNotes();
                    break;
                case IDM_TRAY_RESTART_ADMIN:
                    RestartAsAdmin(hwnd);
                    break;
                case IDM_WAIT_50: case IDM_WAIT_100: case IDM_WAIT_200:
                case IDM_WAIT_300: case IDM_WAIT_500: case IDM_WAIT_800:
                case IDM_WAIT_1000:
                    g_minimizedWaitMs = WaitMsFromId(LOWORD(wParam));
                    SaveSettings();
                    break;
                case IDM_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        }
        
        case WM_DESTROY: {
            DestroyAllStickyNotes();
            DestroyControlPanel();
            UnregisterHotKeys(hwnd);
            
            // 移除托盘图标
            NOTIFYICONDATA nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = IDI_TRAY_ICON;
            Shell_NotifyIcon(NIM_DELETE, &nid);
            
            if (g_hFont) {
                DeleteObject(g_hFont);
                g_hFont = nullptr;
            }
            
            PostQuitMessage(0);
            return 0;
        }
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ─── 注册主窗口类 ───

static ATOM RegisterMainClass() {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"WinTopPreviewMain";
    
    return RegisterClassEx(&wc);
}

// ─── 解析命令行参数 ───

static HWND ParseTargetWindowHandle(int argc, wchar_t* argv[]) {
    if (argc < 2) return nullptr;
    
    // 尝试解析为十六进制句柄
    std::wstring arg = argv[1];
    if (arg.substr(0, 2) == L"0x" || arg.substr(0, 2) == L"0X") {
        arg = arg.substr(2);
    }
    
    try {
        unsigned long long handle = std::stoull(arg, nullptr, 16);
        HWND hwnd = reinterpret_cast<HWND>(handle);
        
        // 验证窗口是否有效
        if (IsWindow(hwnd) && IsWindowVisible(hwnd)) {
            return hwnd;
        }
    } catch (...) {
        // 解析失败
    }
    
    return nullptr;
}

// ─── 显示帮助信息 ───

static void ShowHelp() {
    MessageBox(nullptr,
        L"WinTop Preview - 桌面窗口预览便签\n\n"
        L"用法:\n"
        L"  WinTopPreview.exe [窗口句柄]\n\n"
        L"示例:\n"
        L"  WinTopPreview.exe 0x00123ABC\n"
        L"  WinTopPreview.exe 1234567\n\n"
        L"全局热键:\n"
        L"  Ctrl+Alt+N  新建便签\n"
        L"  Ctrl+Alt+X  销毁所有便签\n\n"
        L"说明:\n"
        L"  - 启动时指定窗口句柄，自动创建预览便签\n"
        L"  - 便签定时刷新（默认 5 秒）\n"
        L"  - 画面停滞超过阈值（默认 10 秒）时变红\n"
        L"  - 可拖拽、缩放便签窗口",
        L"帮助", MB_OK | MB_ICONINFORMATION);
}

// ─── 程序入口 ───

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    g_hInst = hInstance;
    
    // 加载设置（最小化恢复抓帧等待时长等）
    LoadSettings();
    
    // 解析命令行
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    
    HWND targetHwnd = ParseTargetWindowHandle(argc, argv);
    
    if (argc > 1 && !targetHwnd) {
        ShowHelp();
        return 1;
    }
    
    // 初始化 COM
    CoInitialize(nullptr);
    
    // 创建字体
    g_hFont = CreateFont(
        16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei"
    );
    
    // 注册窗口类
    if (!RegisterMainClass()) {
        MessageBox(nullptr, L"注册窗口类失败", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // 创建主窗口（隐藏）
    g_hMainWnd = CreateWindowEx(
        0, L"WinTopPreviewMain", L"WinTop Preview",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
        nullptr, nullptr, g_hInst, nullptr
    );
    
    if (!g_hMainWnd) {
        MessageBox(nullptr, L"创建主窗口失败", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // 注册全局热键
    RegisterHotKeys(g_hMainWnd);
    
    // 添加系统托盘图标
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hMainWnd;
    nid.uID = IDI_TRAY_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_ICON;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"WinTop Preview - 窗口预览工具");
    Shell_NotifyIcon(NIM_ADD, &nid);
    
    // 创建控制面板（默认显示）
    CreateControlPanel(g_hMainWnd);
    
    // 如果指定了目标窗口，创建便签；否则直接打开窗口选择器
    if (targetHwnd) {
        CreateAndBindStickyNote(targetHwnd);
    } else {
        // 没有参数，直接打开窗口选择器
        ShowWindowPicker(g_hMainWnd);
    }
    
    // 消息循环
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    CoUninitialize();
    
    return static_cast<int>(msg.wParam);
}
