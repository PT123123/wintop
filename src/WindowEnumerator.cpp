#include "wintop.h"

// ─── 窗口枚举回调 ───

struct EnumData {
    std::vector<WindowInfo>* windows;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    EnumData* data = reinterpret_cast<EnumData*>(lParam);
    
    // 过滤条件：可见、有标题、非工具窗口
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLength(hwnd) == 0) return TRUE;
    
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
    if (!(exStyle & WS_EX_APPWINDOW) && GetParent(hwnd) != nullptr) return TRUE;
    
    // 获取窗口标题
    int len = GetWindowTextLength(hwnd) + 1;
    std::wstring title(len, L'\0');
    GetWindowText(hwnd, &title[0], len);
    title.resize(len - 1);
    
    // 获取进程 ID
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    
    data->windows->push_back({hwnd, title, processId});
    return TRUE;
}

// ─── 枚举可见窗口 ───

std::vector<WindowInfo> EnumerateVisibleWindows() {
    std::vector<WindowInfo> windows;
    EnumData data{&windows};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
    return windows;
}
