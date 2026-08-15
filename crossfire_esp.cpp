// crossfire_esp.cpp
// Hardcoded placeholder offsets for testing
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ==================== Global Constants ====================
constexpr wchar_t kDefaultTarget[] = L"crossfire.exe";
constexpr int kMaxEntities = 32;
constexpr std::size_t kEntityStride = 0x100;

// ==================== Offsets (Zero Mode) ====================
// 这些地址填 0 就不会尝试读取内存，直接进入绘制循环画个框测试
uintptr_t g_entityList = 0x00000000;
uintptr_t g_viewMatrix = 0x00000000;
uintptr_t g_position = 0x00000000;
uintptr_t g_entityCount = 0x00000000;
int g_entityStride = kEntityStride;
int g_maxEntities = kMaxEntities;

// ==================== Process & Window Helpers ====================
DWORD GetProcessId(const wchar_t* processName) {
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, processName) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return pid;
}

HWND GetWindowHandle(DWORD pid) {
    HWND hwnd = nullptr;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p == static_cast<DWORD>(l)) {
            *reinterpret_cast<HWND*>(lParam) = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hwnd));
    return hwnd;
}

// ==================== Overlay Drawing (GDI) ====================
class OverlayWindow {
public:
    OverlayWindow(HWND targetHwnd) : targetHwnd_(targetHwnd) {
        RECT rect;
        GetClientRect(targetHwnd, &rect);

        // Create a transparent overlay window
        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"STATIC", nullptr,
            WS_POPUP,
            rect.left, rect.top, rect.right, rect.bottom,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
        );
        SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), 255, LWA_COLORKEY);
        ShowWindow(hwnd_, SW_SHOW);

        hdc_ = GetDC(hwnd_);
    }

    ~OverlayWindow() {
        if (hdc_) ReleaseDC(hwnd_, hdc_);
        if (hwnd_) DestroyWindow(hwnd_);
    }

    void DrawBox(int x, int y, int width, int height, COLORREF color = RGB(0, 255, 0)) {
        HPEN pen = CreatePen(PS_SOLID, 2, color);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc_, pen));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc_, GetStockObject(NULL_BRUSH)));

        Rectangle(hdc_, x, y, x + width, y + height);

        SelectObject(hdc_, oldBrush);
        SelectObject(hdc_, oldPen);
        DeleteObject(pen);
    }

    void Clear() {
        RECT rect;
        GetClientRect(hwnd_, &rect);
        FillRect(hdc_, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }

private:
    HWND targetHwnd_ = nullptr;
    HWND hwnd_ = nullptr;
    HDC hdc_ = nullptr;
};

// ==================== Main Loop ====================
int main() {
    // 1. Get game process
    DWORD pid = GetProcessId(kDefaultTarget);
    if (!pid) {
        std::wcerr << L"Target process not found: " << kDefaultTarget << std::endl;
        return 1;
    }
    HWND gameHwnd = GetWindowHandle(pid);
    if (!gameHwnd) {
        std::wcerr << L"Could not locate game window" << std::endl;
        return 1;
    }

    // 2. Set up overlay
    OverlayWindow overlay(gameHwnd);
    std::cout << "Overlay ready. Press Ctrl+C to exit." << std::endl;

    // 3. Main loop (test drawing)
    while (true) {
        overlay.Clear();

        // 测试模式：什么都不读，只画一个测试框
        // 只要屏幕上出现这个绿框，就说明反作弊没有拦截画图引擎
        overlay.DrawBox(100, 100, 200, 200, RGB(0, 255, 0));

        // Prevent high CPU usage
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    return 0;
}
