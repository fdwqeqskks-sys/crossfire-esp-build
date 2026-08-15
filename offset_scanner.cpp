#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cwchar>
#include <iomanip>
#include <iostream>

namespace {

constexpr wchar_t kProcessName[] = L"crossfire.exe";

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle) {}

    ~UniqueHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const {
        return handle_;
    }

    explicit operator bool() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

void WaitForExit() {
    std::cout << "Press Enter to exit...";
    std::cin.get();
}

DWORD FindProcessId() {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        std::cerr << "CreateToolhelp32Snapshot(process) failed, Win32="
                  << GetLastError() << '\n';
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));

    if (!Process32FirstW(snapshot.get(), &entry)) {
        std::cerr << "Process32FirstW failed, Win32="
                  << GetLastError() << '\n';
        return 0;
    }

    do {
        if (_wcsicmp(entry.szExeFile, kProcessName) == 0) {
            return entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot.get(), &entry));

    return 0;
}

std::uintptr_t FindModuleBase(DWORD processId) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        processId
    ));

    if (!snapshot) {
        std::cerr << "CreateToolhelp32Snapshot(module) failed, Win32="
                  << GetLastError() << '\n';
        return 0;
    }

    MODULEENTRY32W module{};
    module.dwSize = static_cast<DWORD>(sizeof(module));

    if (!Module32FirstW(snapshot.get(), &module)) {
        std::cerr << "Module32FirstW failed, Win32="
                  << GetLastError() << '\n';
        return 0;
    }

    do {
        if (_wcsicmp(module.szModule, kProcessName) == 0 ||
            _wcsicmp(module.szExePath, kProcessName) == 0) {
            return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
        }
    } while (Module32NextW(snapshot.get(), &module));

    return 0;
}

}  // namespace

int main() {
    std::cout << "Looking for crossfire.exe...\n";

    const DWORD processId = FindProcessId();
    if (processId == 0) {
        std::cerr << "crossfire.exe was not found.\n";
        WaitForExit();
        return 1;
    }

    std::cout << "PID: " << processId << '\n';

    const std::uintptr_t moduleBase = FindModuleBase(processId);
    if (moduleBase == 0) {
        std::cerr << "The module base address was not available.\n";
        WaitForExit();
        return 2;
    }

    std::cout << "Module base: 0x"
              << std::hex << std::uppercase << moduleBase
              << std::dec << '\n';

    WaitForExit();
    return 0;
}
