#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cwchar>

constexpr wchar_t kProcessName[] = L"crossfire.exe";
constexpr wchar_t kOverlayClass[] = L"CrossfireTestOverlay";

constexpr std::uintptr_t kEntityListOffset = 0x00000000;
constexpr std::uintptr_t kEntityCountOffset = 0x00000000;
constexpr std::uintptr_t kViewMatrixOffset = 0x00000000;
constexpr std::uintptr_t kPositionOffset = 0x00000000;
constexpr std::uintptr_t kHealthOffset = 0x00000000;
constexpr std::uintptr_t kTeamOffset = 0x00000000;

struct WindowSearch {
    DWORD processId;
    HWND window;
};

DWORD FindProcessId(const wchar_t* processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD processId = 0;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                processId = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processId;
}

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);

    if (processId != search->processId) {
        return TRUE;
    }

    if (!IsWindowVisible(window)) {
        return TRUE;
    }

    if (GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }

    RECT rectangle{};
    if (!GetClientRect(window, &rectangle)) {
        return TRUE;
    }

    if (rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return TRUE;
    }

    search->window = window;
    return FALSE;
}

HWND FindProcessWindow(DWORD processId) {
    WindowSearch search{processId, nullptr};
    EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

bool PositionOverlay(HWND overlay, HWND target) {
    RECT client{};
    if (!GetClientRect(target, &client)) {
        return false;
    }

    POINT origin{0, 0};
    if (!ClientToScreen(target, &origin)) {
        return false;
    }

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    if (width <= 0 || height <= 0) {
        return false;
    }

    return SetWindowPos(
               overlay,
               HWND_TOPMOST,
               origin.x,
               origin.y,
               width,
               height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW
           ) != FALSE;
}

void DrawTestBoxes(HDC target, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int centerX = width / 2;
    const int centerY = height / 2;

    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);

    FillRect(
        buffer,
        &client,
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))
    );

    HPEN greenPen = CreatePen(PS_SOLID, 3, RGB(0, 255, 0));
    HPEN redPen = CreatePen(PS_SOLID, 2, RGB(255, 64, 64));
    HGDIOBJ oldPen = SelectObject(buffer, greenPen);
    HGDIOBJ oldBrush = SelectObject(
        buffer,
        GetStockObject(HOLLOW_BRUSH)
    );

    Rectangle(
        buffer,
        centerX - 55,
        centerY - 130,
        centerX + 55,
        centerY + 130
    );

    SelectObject(buffer, redPen);

    Rectangle(
        buffer,
        centerX - 230,
        centerY - 105,
        centerX - 140,
        centerY + 105
    );

    Rectangle(
        buffer,
        centerX + 140,
        centerY - 105,
        centerX + 230,
        centerY + 105
    );

    MoveToEx(buffer, centerX - 12, centerY, nullptr);
    LineTo(buffer, centerX + 13, centerY);
    MoveToEx(buffer, centerX, centerY - 12, nullptr);
    LineTo(buffer, centerX, centerY + 13);

    BitBlt(
        target,
        0,
        0,
        width,
        height,
        buffer,
        0,
        0,
        SRCCOPY
    );

    SelectObject(buffer, oldBrush);
    SelectObject(buffer, oldPen);
    SelectObject(buffer, oldBitmap);

    DeleteObject(redPen);
    DeleteObject(greenPen);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

LRESULT CALLBACK OverlayWindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC device = BeginPaint(window, &paint);

        RECT client{};
        GetClientRect(window, &client);
        DrawTestBoxes(device, client);

        EndPaint(window, &paint);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

HWND CreateOverlay(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = OverlayWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kOverlayClass;

    if (!RegisterClassExW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }

    HWND overlay = CreateWindowExW(
        WS_EX_LAYERED |
            WS_EX_TRANSPARENT |
            WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        kOverlayClass,
        L"Crossfire Test Overlay",
        WS_POPUP,
        0,
        0,
        100,
        100,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!overlay) {
        return nullptr;
    }

    SetLayeredWindowAttributes(
        overlay,
        RGB(0, 0, 0),
        0,
        LWA_COLORKEY
    );

    ShowWindow(overlay, SW_SHOWNA);
    UpdateWindow(overlay);
    return overlay;
}

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int
) {
    SetProcessDPIAware();

    volatile std::uintptr_t offsets[] = {
        kEntityListOffset,
        kEntityCountOffset,
        kViewMatrixOffset,
        kPositionOffset,
        kHealthOffset,
        kTeamOffset
    };
    static_cast<void>(offsets);

    DWORD processId = 0;
    HWND targetWindow = nullptr;

    while (!processId || !targetWindow) {
        processId = FindProcessId(kProcessName);

        if (processId) {
            targetWindow = FindProcessWindow(processId);
        }

        if (GetAsyncKeyState(VK_END) & 1) {
            return 0;
        }

        Sleep(500);
    }

    HWND overlay = CreateOverlay(instance);
    if (!overlay) {
        return 1;
    }

    MSG message{};
    bool running = true;

    while (running) {
        while (PeekMessageW(
            &message,
            nullptr,
            0,
            0,
            PM_REMOVE
        )) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (!running ||
            !IsWindow(targetWindow) ||
            FindProcessId(kProcessName) != processId) {
            break;
        }

        if (GetAsyncKeyState(VK_END) & 1) {
            break;
        }

        PositionOverlay(overlay, targetWindow);
        InvalidateRect(overlay, nullptr, FALSE);
        UpdateWindow(overlay);
        Sleep(16);
    }

    DestroyWindow(overlay);
    return 0;
}
