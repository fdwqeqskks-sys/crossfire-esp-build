#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>

namespace {

constexpr wchar_t kProcessName[] = L"crossfire.exe";
constexpr ULONG kProcessBasicInformation = 0;
constexpr ULONG kProcessWow64Information = 26;

using NtQueryInformationProcessFn = LONG(NTAPI*)(
    HANDLE, ULONG, PVOID, ULONG, PULONG
);

struct ProcessBasicInformation {
    LONG exitStatus;
    PVOID pebBaseAddress;
    ULONG_PTR affinityMask;
    LONG basePriority;
    ULONG_PTR uniqueProcessId;
    ULONG_PTR inheritedFromUniqueProcessId;
};

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

    HANDLE get() const { return handle_; }
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
        std::cerr << "Process snapshot failed, Win32=" << GetLastError() << '\n';
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));

    if (!Process32FirstW(snapshot.get(), &entry)) {
        std::cerr << "Process32FirstW failed, Win32="
                  << GetLastError() << '\n';
        std::cerr << "Process32FirstW failed, Win32=" << GetLastError() << '\n';
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
NtQueryInformationProcessFn ResolveNtQueryInformationProcess() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return nullptr;
    }

    MODULEENTRY32W module{};
    module.dwSize = static_cast<DWORD>(sizeof(module));
    const FARPROC address = GetProcAddress(ntdll, "NtQueryInformationProcess");
    NtQueryInformationProcessFn function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

    if (!Module32FirstW(snapshot.get(), &module)) {
        std::cerr << "Module32FirstW failed, Win32="
                  << GetLastError() << '\n';
template <typename T>
bool ReadRemote(HANDLE process, std::uintptr_t address, T& value) {
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(address),
            &value,
            sizeof(value),
            &bytesRead
        ) || bytesRead != sizeof(value)) {
        std::cerr << "ReadProcessMemory failed, Win32=" << GetLastError() << '\n';
        return false;
    }
    return true;
}

std::uintptr_t FindImageBase(
    HANDLE process,
    NtQueryInformationProcessFn ntQuery
) {
    BOOL isWow64 = FALSE;
    if (!IsWow64Process(process, &isWow64)) {
        std::cerr << "IsWow64Process failed, Win32=" << GetLastError() << '\n';
        return 0;
    }

    do {
        if (_wcsicmp(module.szModule, kProcessName) == 0 ||
            _wcsicmp(module.szExePath, kProcessName) == 0) {
            return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
    if (isWow64) {
        ULONG_PTR peb32 = 0;
        const LONG status = ntQuery(
            process,
            kProcessWow64Information,
            &peb32,
            static_cast<ULONG>(sizeof(peb32)),
            nullptr
        );
        if (status < 0 || peb32 == 0) {
            std::cerr << "NtQueryInformationProcess(WOW64) failed, NTSTATUS=0x"
                      << std::hex << std::uppercase
                      << static_cast<std::uint32_t>(status) << std::dec << '\n';
            return 0;
        }
    } while (Module32NextW(snapshot.get(), &module));

        std::uint32_t imageBase = 0;
        if (!ReadRemote(process, static_cast<std::uintptr_t>(peb32) + 0x08, imageBase)) {
    return 0;
            return 0;
        }
        return static_cast<std::uintptr_t>(imageBase);
    }

    ProcessBasicInformation information{};
    const LONG status = ntQuery(
        process,
        kProcessBasicInformation,
        &information,
        static_cast<ULONG>(sizeof(information)),
        nullptr
    );
    if (status < 0 || information.pebBaseAddress == nullptr) {
        std::cerr << "NtQueryInformationProcess failed, NTSTATUS=0x"
                  << std::hex << std::uppercase
                  << static_cast<std::uint32_t>(status) << std::dec << '\n';
        return 0;
    }

    std::uintptr_t imageBase = 0;
    const auto peb = reinterpret_cast<std::uintptr_t>(information.pebBaseAddress);
    if (!ReadRemote(process, peb + 0x10, imageBase)) {
        return 0;
    }
    return imageBase;
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
    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        processId
    ));
    if (!process) {
        std::cerr << "OpenProcess failed, Win32=" << GetLastError() << '\n';
        WaitForExit();
        return 2;
    }

    std::cout << "Module base: 0x"
              << std::hex << std::uppercase << moduleBase
              << std::dec << '\n';
    const auto ntQuery = ResolveNtQueryInformationProcess();
    if (ntQuery == nullptr) {
        std::cerr << "NtQueryInformationProcess was not found.\n";
        WaitForExit();
        return 3;
    }

    const std::uintptr_t imageBase = FindImageBase(process.get(), ntQuery);
    if (imageBase == 0) {
        std::cerr << "The image base address was not available.\n";
        WaitForExit();
        return 4;
    }

    std::cout << "Module base: 0x"
              << std::hex << std::uppercase << imageBase << std::dec << '\n';
    WaitForExit();
    return 0;
}
