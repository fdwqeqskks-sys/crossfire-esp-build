#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kDefaultTarget[] = L"crossfire.exe";
constexpr LONG kStatusSuccess = 0;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004L);
constexpr ULONG kSystemExtendedHandleInformation = 64;

using NtReadVirtualMemoryFn = LONG(NTAPI*)(
    HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
using NtQuerySystemInformationFn = LONG(NTAPI*)(
    ULONG, PVOID, ULONG, PULONG);

struct SystemHandleEntryEx {
    PVOID object;
    ULONG_PTR ownerPid;
    ULONG_PTR handleValue;
    ULONG grantedAccess;
    USHORT creatorBackTraceIndex;
    USHORT objectTypeIndex;
    ULONG handleAttributes;
    ULONG reserved;
};

struct SystemHandleInformationEx {
    ULONG_PTR handleCount;
    ULONG_PTR reserved;
    SystemHandleEntryEx handles[1];
};

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const { return handle_; }
    explicit operator bool() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() {
        HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }
    void reset(HANDLE replacement = nullptr) {
        if (*this) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Matrix4x4 {
    float value[16];
};

struct ScreenPoint {
    float x;
    float y;
};

struct Config {
    std::wstring target = kDefaultTarget;
    std::uintptr_t entityListRva = 0;
    std::uintptr_t entityCountRva = 0;
    std::uintptr_t viewMatrixRva = 0;
    std::uintptr_t positionOffset = 0;
    std::uintptr_t healthOffset = 0;
    std::uintptr_t teamOffset = 0;
    std::size_t entityStride = 0;
    std::uint32_t maxEntities = 64;
    std::uint32_t pointerSize = 0;
    bool entityListDirect = false;
    bool viewMatrixPointer = false;
    bool verticalAxisY = false;
};

struct Runtime {
    Config config;
    UniqueHandle process;
    DWORD processId = 0;
    std::uintptr_t moduleBase = 0;
    HWND gameWindow = nullptr;
};

NtReadVirtualMemoryFn g_ntReadVirtualMemory = nullptr;
NtQuerySystemInformationFn g_ntQuerySystemInformation = nullptr;
Runtime* g_runtime = nullptr;
std::ofstream g_log;

std::string Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::ostringstream stream;
    stream << std::setfill('0')
           << std::setw(2) << time.wHour << ':'
           << std::setw(2) << time.wMinute << ':'
           << std::setw(2) << time.wSecond << '.'
           << std::setw(3) << time.wMilliseconds;
    return stream.str();
}

void Log(const std::string& message) {
    const std::string line = '[' + Timestamp() + "] " + message;
    std::cout << line << '\n';
    if (g_log) {
        g_log << line << '\n';
        g_log.flush();
    }
}

std::string Hex(std::uintptr_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

std::optional<std::uintptr_t> ParseNumber(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 0);
    if (end == text || *end != L'\0') {
        return std::nullopt;
    }
    return static_cast<std::uintptr_t>(value);
}

bool ParseArguments(int argc, wchar_t** argv, Config& config) {
    const std::unordered_map<std::wstring, std::uintptr_t*> offsets = {
        {L"--entity-list", &config.entityListRva},
        {L"--entity-count", &config.entityCountRva},
        {L"--view-matrix", &config.viewMatrixRva},
        {L"--position", &config.positionOffset},
        {L"--health", &config.healthOffset},
        {L"--team", &config.teamOffset},
    };

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--entity-list-direct") {
            config.entityListDirect = true;
            continue;
        }
        if (argument == L"--view-matrix-pointer") {
            config.viewMatrixPointer = true;
            continue;
        }
        if (argument == L"--up-axis-y") {
            config.verticalAxisY = true;
            continue;
        }
        if (argument == L"--target" && index + 1 < argc) {
            config.target = argv[++index];
            continue;
        }

        const auto offset = offsets.find(argument);
        if (offset != offsets.end() && index + 1 < argc) {
            const auto value = ParseNumber(argv[++index]);
            if (!value) {
                Log("Invalid numeric argument for " +
                    std::string(argument.begin(), argument.end()));
                return false;
            }
            *offset->second = *value;
            continue;
        }

        if ((argument == L"--stride" || argument == L"--max-entities" ||
             argument == L"--pointer-size") && index + 1 < argc) {
            const auto value = ParseNumber(argv[++index]);
            if (!value) {
                Log("Invalid numeric option");
                return false;
            }
            if (argument == L"--stride") {
                config.entityStride = static_cast<std::size_t>(*value);
            } else if (argument == L"--max-entities") {
                config.maxEntities = static_cast<std::uint32_t>(*value);
            } else {
                config.pointerSize = static_cast<std::uint32_t>(*value);
            }
            continue;
        }

        Log("Unknown or incomplete argument: " +
            std::string(argument.begin(), argument.end()));
        return false;
    }
    return true;
}

bool EnableDebugPrivilege() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &rawToken)) {
        Log("OpenProcessToken failed: Win32=" +
            std::to_string(GetLastError()));
        return false;
    }
    UniqueHandle token(rawToken);

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        Log("LookupPrivilegeValueW failed: Win32=" +
            std::to_string(GetLastError()));
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token.get(), FALSE, &privileges,
                               sizeof(privileges), nullptr, nullptr) ||
        GetLastError() != ERROR_SUCCESS) {
        Log("SeDebugPrivilege unavailable: Win32=" +
            std::to_string(GetLastError()));
        return false;
    }
    Log("SeDebugPrivilege enabled");
    return true;
}

DWORD FindProcessId(const std::wstring& processName) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return 0;
    }

    do {
        if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0) {
            return entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot.get(), &entry));
    return 0;
}

std::uintptr_t FindModuleBase(DWORD processId,
                              const std::wstring& moduleName) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId));
    if (!snapshot) {
        Log("Module snapshot failed: Win32=" +
            std::to_string(GetLastError()));
        return 0;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
        return 0;
    }

    do {
        if (_wcsicmp(entry.szModule, moduleName.c_str()) == 0) {
            return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        }
    } while (Module32NextW(snapshot.get(), &entry));
    return 0;
}

bool ReadMemory(HANDLE process, std::uintptr_t address,
                void* output, std::size_t size) {
    if (!g_ntReadVirtualMemory || !process || !address || !output || !size) {
        return false;
    }
    SIZE_T transferred = 0;
    const LONG status = g_ntReadVirtualMemory(
        process, reinterpret_cast<PVOID>(address), output, size, &transferred);
    return status == kStatusSuccess && transferred == size;
}

template <typename T>
bool ReadValue(HANDLE process, std::uintptr_t address, T& output) {
    return ReadMemory(process, address, &output, sizeof(output));
}

bool ReadPointer(HANDLE process, std::uintptr_t address,
                 std::uint32_t pointerSize, std::uintptr_t& output) {
    if (pointerSize == 4) {
        std::uint32_t value = 0;
        if (!ReadValue(process, address, value)) {
            return false;
        }
        output = value;
        return true;
    }
    std::uint64_t value = 0;
    if (!ReadValue(process, address, value)) {
        return false;
    }
    output = static_cast<std::uintptr_t>(value);
    return true;
}

bool ValidateReadHandle(HANDLE handle, DWORD processId,
                        std::uintptr_t moduleBase) {
    if (!handle || GetProcessId(handle) != processId) {
        return false;
    }
    WORD signature = 0;
    return ReadValue(handle, moduleBase, signature) && signature == 0x5A4D;
}

UniqueHandle DuplicateExistingTargetHandle(DWORD targetPid,
                                           std::uintptr_t moduleBase) {
    if (!g_ntQuerySystemInformation) {
        return {};
    }

    std::vector<std::byte> buffer(1 << 20);
    ULONG required = 0;
    LONG status = 0;
    for (;;) {
        status = g_ntQuerySystemInformation(
            kSystemExtendedHandleInformation, buffer.data(),
            static_cast<ULONG>(buffer.size()), &required);
        if (status != kStatusInfoLengthMismatch) {
            break;
        }
        buffer.resize(std::max<std::size_t>(buffer.size() * 2,
                                            static_cast<std::size_t>(required) +
                                                (1 << 20)));
    }
    if (status != kStatusSuccess) {
        Log("NtQuerySystemInformation failed: NTSTATUS=" +
            Hex(static_cast<std::uint32_t>(status)));
        return {};
    }

    const auto* information =
        reinterpret_cast<const SystemHandleInformationEx*>(buffer.data());
    const DWORD currentPid = GetCurrentProcessId();
    DWORD cachedOwnerPid = 0;
    UniqueHandle cachedOwner;

    for (ULONG_PTR index = 0; index < information->handleCount; ++index) {
        const auto& entry = information->handles[index];
        const DWORD ownerPid = static_cast<DWORD>(entry.ownerPid);
        if (!ownerPid || ownerPid == currentPid || ownerPid == targetPid) {
            continue;
        }

        if (ownerPid != cachedOwnerPid) {
            cachedOwner.reset(OpenProcess(PROCESS_DUP_HANDLE, FALSE, ownerPid));
            cachedOwnerPid = ownerPid;
        }
        if (!cachedOwner) {
            continue;
        }

        HANDLE duplicated = nullptr;
        if (!DuplicateHandle(
                cachedOwner.get(),
                reinterpret_cast<HANDLE>(entry.handleValue),
                GetCurrentProcess(), &duplicated, 0, FALSE,
                DUPLICATE_SAME_ACCESS)) {
            continue;
        }
        UniqueHandle candidate(duplicated);
        if (ValidateReadHandle(candidate.get(), targetPid, moduleBase)) {
            Log("Reused an existing target handle from PID " +
                std::to_string(ownerPid));
            return candidate;
        }
    }

    Log("Existing-handle fallback exhausted");
    return {};
}

UniqueHandle AcquireTargetHandle(DWORD processId,
                                 std::uintptr_t moduleBase) {
    UniqueHandle direct(OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, processId));
    if (direct && ValidateReadHandle(direct.get(), processId, moduleBase)) {
        Log("Direct process handle acquired");
        return direct;
    }

    Log("Direct OpenProcess/read failed: Win32=" +
        std::to_string(GetLastError()));
    return DuplicateExistingTargetHandle(processId, moduleBase);
}

bool DetectPointerSize(HANDLE process, DWORD processId,
                       std::uint32_t& pointerSize) {
    if (pointerSize == 4 || pointerSize == 8) {
        return true;
    }

    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                       "IsWow64Process2"));
    if (isWow64Process2 &&
        isWow64Process2(process, &processMachine, &nativeMachine)) {
        pointerSize = processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? 8 : 4;
    } else {
        BOOL wow64 = FALSE;
        if (!IsWow64Process(process, &wow64)) {
            Log("Architecture detection failed for PID " +
                std::to_string(processId));
            return false;
        }
        pointerSize = wow64 ? 4 : 8;
    }
    Log("Remote pointer size: " + std::to_string(pointerSize));
    return true;
}

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter) {
    auto* data = reinterpret_cast<std::pair<DWORD, HWND*>*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == data->first && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == nullptr) {
        *data->second = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindGameWindow(DWORD processId) {
    HWND result = nullptr;
    std::pair<DWORD, HWND*> data{processId, &result};
    EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&data));
    return result;
}

bool WorldToScreen(const Vec3& position, const Matrix4x4& matrix,
                   int width, int height, ScreenPoint& screen) {
    const float clipX = position.x * matrix.value[0] +
                        position.y * matrix.value[4] +
                        position.z * matrix.value[8] + matrix.value[12];
    const float clipY = position.x * matrix.value[1] +
                        position.y * matrix.value[5] +
                        position.z * matrix.value[9] + matrix.value[13];
    const float clipW = position.x * matrix.value[3] +
                        position.y * matrix.value[7] +
                        position.z * matrix.value[11] + matrix.value[15];
    if (!std::isfinite(clipW) || clipW < 0.01f) {
        return false;
    }
    const float normalizedX = clipX / clipW;
    const float normalizedY = clipY / clipW;
    screen.x = (width * 0.5f) * (normalizedX + 1.0f);
    screen.y = (height * 0.5f) * (1.0f - normalizedY);
    return std::isfinite(screen.x) && std::isfinite(screen.y);
}

bool ReadViewMatrix(const Runtime& runtime, Matrix4x4& matrix) {
    std::uintptr_t address = runtime.moduleBase +
                             runtime.config.viewMatrixRva;
    if (runtime.config.viewMatrixPointer &&
        !ReadPointer(runtime.process.get(), address,
                     runtime.config.pointerSize, address)) {
        return false;
    }
    return ReadValue(runtime.process.get(), address, matrix);
}

void DrawEntities(HDC device, int width, int height) {
    if (!g_runtime) {
        return;
    }
    const Runtime& runtime = *g_runtime;
    Matrix4x4 matrix{};
    if (!ReadViewMatrix(runtime, matrix)) {
        return;
    }

    std::uintptr_t listAddress = runtime.moduleBase +
                                 runtime.config.entityListRva;
    if (!runtime.config.entityListDirect &&
        !ReadPointer(runtime.process.get(), listAddress,
                     runtime.config.pointerSize, listAddress)) {
        return;
    }

    std::uint32_t count = runtime.config.maxEntities;
    if (runtime.config.entityCountRva) {
        ReadValue(runtime.process.get(),
                  runtime.moduleBase + runtime.config.entityCountRva, count);
        count = std::min(count, runtime.config.maxEntities);
    }

    const std::size_t stride = runtime.config.entityStride
                                   ? runtime.config.entityStride
                                   : runtime.config.pointerSize;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 64, 64));
    HGDIOBJ oldPen = SelectObject(device, pen);
    HGDIOBJ oldBrush = SelectObject(device, GetStockObject(HOLLOW_BRUSH));
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, RGB(255, 255, 255));

    for (std::uint32_t index = 0; index < count; ++index) {
        std::uintptr_t entity = 0;
        if (!ReadPointer(runtime.process.get(), listAddress + index * stride,
                         runtime.config.pointerSize, entity) || !entity) {
            continue;
        }

        Vec3 feet{};
        if (!ReadValue(runtime.process.get(),
                       entity + runtime.config.positionOffset, feet) ||
            !std::isfinite(feet.x) || !std::isfinite(feet.y) ||
            !std::isfinite(feet.z) || std::abs(feet.x) > 1.0e6f ||
            std::abs(feet.y) > 1.0e6f || std::abs(feet.z) > 1.0e6f) {
            continue;
        }

        int health = 1;
        if (runtime.config.healthOffset &&
            (!ReadValue(runtime.process.get(),
                        entity + runtime.config.healthOffset, health) ||
             health <= 0 || health > 100000)) {
            continue;
        }

        Vec3 head = feet;
        if (runtime.config.verticalAxisY) {
            head.y += 1.8f;
        } else {
            head.z += 1.8f;
        }

        ScreenPoint feetScreen{};
        ScreenPoint headScreen{};
        if (!WorldToScreen(feet, matrix, width, height, feetScreen) ||
            !WorldToScreen(head, matrix, width, height, headScreen)) {
            continue;
        }

        const float boxHeight = std::abs(feetScreen.y - headScreen.y);
        if (boxHeight < 4.0f || boxHeight > height * 2.0f) {
            continue;
        }
        const float boxWidth = boxHeight * 0.45f;
        const int left = static_cast<int>(headScreen.x - boxWidth * 0.5f);
        const int top = static_cast<int>(headScreen.y);
        const int right = static_cast<int>(headScreen.x + boxWidth * 0.5f);
        const int bottom = static_cast<int>(feetScreen.y);
        Rectangle(device, left, top, right, bottom);

        const std::wstring label = runtime.config.healthOffset
                                       ? L"HP " + std::to_wstring(health)
                                       : L"PLAYER";
        TextOutW(device, left, std::max(0, top - 18), label.c_str(),
                 static_cast<int>(label.size()));
    }

    SelectObject(device, oldBrush);
    SelectObject(device, oldPen);
    DeleteObject(pen);
}

LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message,
                                   WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        HDC buffer = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
        FillRect(buffer, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        DrawEntities(buffer, width, height);
        BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
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
    constexpr wchar_t className[] = L"CrossfireDiagnosticOverlay";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = OverlayWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }

    HWND overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        className, L"Diagnostic Overlay", WS_POPUP,
        0, 0, 100, 100, nullptr, nullptr, instance, nullptr);
    if (overlay) {
        SetLayeredWindowAttributes(overlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
        ShowWindow(overlay, SW_SHOWNA);
    }
    return overlay;
}

bool PositionOverlay(HWND overlay, HWND gameWindow) {
    RECT client{};
    if (!GetClientRect(gameWindow, &client)) {
        return false;
    }
    POINT origin{client.left, client.top};
    if (!ClientToScreen(gameWindow, &origin)) {
        return false;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return false;
    }
    return SetWindowPos(overlay, HWND_TOPMOST, origin.x, origin.y,
                        width, height,
                        SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
}

void PrintUsage() {
    Log("Required version-specific arguments:");
    Log("  --entity-list OFFSET --view-matrix OFFSET --position OFFSET");
    Log("Optional arguments:");
    Log("  --entity-count OFFSET --health OFFSET --team OFFSET");
    Log("  --stride N --max-entities N --pointer-size 4|8");
    Log("  --entity-list-direct --view-matrix-pointer --up-axis-y");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    g_log.open("crossfire_esp.log", std::ios::out | std::ios::trunc);
    Log("Starting x64 memory/overlay diagnostic");

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_ntReadVirtualMemory = reinterpret_cast<NtReadVirtualMemoryFn>(
        GetProcAddress(ntdll, "NtReadVirtualMemory"));
    g_ntQuerySystemInformation =
        reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!g_ntReadVirtualMemory || !g_ntQuerySystemInformation) {
        Log("Required ntdll exports are missing");
        return 1;
    }

    Runtime runtime;
    if (!ParseArguments(argc, argv, runtime.config)) {
        PrintUsage();
        return 2;
    }
    if (!runtime.config.entityListRva || !runtime.config.viewMatrixRva ||
        !runtime.config.positionOffset) {
        Log("Version-specific offsets are missing");
        PrintUsage();
        Log("SUGGESTION: obtain offsets for the exact target build and rerun");
        return 2;
    }

    EnableDebugPrivilege();
    runtime.processId = FindProcessId(runtime.config.target);
    if (!runtime.processId) {
        Log("Target process was not found");
        return 3;
    }
    Log("Target PID: " + std::to_string(runtime.processId));

    runtime.moduleBase = FindModuleBase(runtime.processId,
                                        runtime.config.target);
    if (!runtime.moduleBase) {
        Log("Main module base was not found");
        Log("SUGGESTION: verify process architecture and module visibility");
        return 4;
    }
    Log("Module base: " + Hex(runtime.moduleBase));

    runtime.process = AcquireTargetHandle(runtime.processId,
                                          runtime.moduleBase);
    if (!runtime.process) {
        Log("All user-mode read-handle methods failed");
        Log("SUGGESTION: kernel callback is stripping read rights");
        return 5;
    }
    if (!DetectPointerSize(runtime.process.get(), runtime.processId,
                           runtime.config.pointerSize)) {
        return 6;
    }

    runtime.gameWindow = FindGameWindow(runtime.processId);
    if (!runtime.gameWindow) {
        Log("Game window was not found");
        return 7;
    }

    HWND overlay = CreateOverlay(GetModuleHandleW(nullptr));
    if (!overlay) {
        Log("Overlay creation failed: Win32=" +
            std::to_string(GetLastError()));
        return 8;
    }

    g_runtime = &runtime;
    Log("Overlay started; runtime log: crossfire_esp.log");
    MSG message{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running || !IsWindow(runtime.gameWindow)) {
            break;
        }
        PositionOverlay(overlay, runtime.gameWindow);
        InvalidateRect(overlay, nullptr, FALSE);
        UpdateWindow(overlay);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    DestroyWindow(overlay);
    g_runtime = nullptr;
    Log("Overlay stopped");
    return 0;
}
