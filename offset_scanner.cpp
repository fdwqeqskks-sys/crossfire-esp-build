#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kTargetName[] = L"crossfire.exe";
constexpr ULONG kSystemProcessInformation = 5;
constexpr ULONG kProcessBasicInformation = 0;
constexpr ULONG kProcessWow64Information = 26;
constexpr LONG kStatusInfoLengthMismatch = -1073741820L;

struct UnicodeString {
    USHORT length;
    USHORT maximumLength;
    PWSTR buffer;
};

struct SystemProcessInformation {
    ULONG nextEntryOffset;
    ULONG numberOfThreads;
    LARGE_INTEGER workingSetPrivateSize;
    ULONG hardFaultCount;
    ULONG numberOfThreadsHighWatermark;
    ULONGLONG cycleTime;
    LARGE_INTEGER createTime;
    LARGE_INTEGER userTime;
    LARGE_INTEGER kernelTime;
    UnicodeString imageName;
    LONG basePriority;
    HANDLE uniqueProcessId;
    HANDLE inheritedProcessId;
};

struct BasicProcessInformation {
    LONG exitStatus;
    PVOID pebBaseAddress;
    ULONG_PTR affinityMask;
    LONG basePriority;
    ULONG_PTR uniqueProcessId;
    ULONG_PTR inheritedProcessId;
};

using NtQuerySystemInformationFn = LONG(NTAPI*)(
    ULONG, PVOID, ULONG, PULONG
);

using NtQueryInformationProcessFn = LONG(NTAPI*)(
    HANDLE, ULONG, PVOID, ULONG, PULONG
);

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE value = nullptr) : value_(value) {}

    ~UniqueHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const {
        return value_;
    }

    explicit operator bool() const {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_;
};

template <typename Function>
Function ResolveNtdllFunction(const char* name) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return nullptr;
    }

    const FARPROC address = GetProcAddress(ntdll, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

void WaitForExit() {
    std::cout << "Press Enter to exit...";
    std::cin.get();
}

DWORD FindProcessId(NtQuerySystemInformationFn querySystem) {
    std::vector<std::byte> buffer(64 * 1024);
    LONG status = 0;

    for (int attempt = 0; attempt < 8; ++attempt) {
        ULONG required = 0;
        status = querySystem(
            kSystemProcessInformation,
            buffer.data(),
            static_cast<ULONG>(buffer.size()),
            &required
        );

        if (status >= 0) {
            break;
        }

        if (status != kStatusInfoLengthMismatch) {
            std::cerr
                << "NtQuerySystemInformation failed, NTSTATUS=0x"
                << std::hex
                << std::uppercase
                << static_cast<std::uint32_t>(status)
                << std::dec
                << '\n';
            return 0;
        }

        const std::size_t nextSize = required != 0
            ? static_cast<std::size_t>(required) + 64 * 1024
            : buffer.size() * 2;

        buffer.resize(nextSize);
    }

    if (status < 0) {
        std::cerr << "The process list buffer could not be allocated.\n";
        return 0;
    }

    std::size_t offset = 0;

    while (offset < buffer.size()) {
        const auto* entry =
            reinterpret_cast<const SystemProcessInformation*>(
                buffer.data() + offset
            );

        if (entry->imageName.buffer != nullptr &&
            entry->imageName.length != 0) {
            const std::wstring name(
                entry->imageName.buffer,
                entry->imageName.length / sizeof(wchar_t)
            );

            if (_wcsicmp(name.c_str(), kTargetName) == 0) {
                return static_cast<DWORD>(
                    reinterpret_cast<ULONG_PTR>(
                        entry->uniqueProcessId
                    )
                );
            }
        }

        if (entry->nextEntryOffset == 0) {
            break;
        }

        offset += entry->nextEntryOffset;
    }

    return 0;
}

template <typename Value>
bool ReadRemote(
    HANDLE process,
    std::uintptr_t address,
    Value& value
) {
    SIZE_T bytesRead = 0;

    if (!ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(address),
            &value,
            sizeof(value),
            &bytesRead
        ) || bytesRead != sizeof(value)) {
        std::cerr
            << "ReadProcessMemory failed, Win32="
            << GetLastError()
            << '\n';
        return false;
    }

    return true;
}

std::uintptr_t FindImageBase(
    HANDLE process,
    NtQueryInformationProcessFn queryProcess
) {
    ULONG_PTR wow64Peb = 0;

    const LONG wow64Status = queryProcess(
        process,
        kProcessWow64Information,
        &wow64Peb,
        static_cast<ULONG>(sizeof(wow64Peb)),
        nullptr
    );

    if (wow64Status >= 0 && wow64Peb != 0) {
        std::uint32_t imageBase32 = 0;

        if (!ReadRemote(
                process,
                wow64Peb + 0x08,
                imageBase32
            )) {
            return 0;
        }

        return static_cast<std::uintptr_t>(imageBase32);
    }

    BasicProcessInformation information{};

    const LONG status = queryProcess(
        process,
        kProcessBasicInformation,
        &information,
        static_cast<ULONG>(sizeof(information)),
        nullptr
    );

    if (status < 0 || information.pebBaseAddress == nullptr) {
        std::cerr
            << "NtQueryInformationProcess failed, NTSTATUS=0x"
            << std::hex
            << std::uppercase
            << static_cast<std::uint32_t>(status)
            << std::dec
            << '\n';
        return 0;
    }

    std::uintptr_t imageBase = 0;

    const auto pebAddress =
        reinterpret_cast<std::uintptr_t>(
            information.pebBaseAddress
        );

    if (!ReadRemote(
            process,
            pebAddress + 0x10,
            imageBase
        )) {
        return 0;
    }

    return imageBase;
}

} // namespace

int main() {
    const auto querySystem =
        ResolveNtdllFunction<NtQuerySystemInformationFn>(
            "NtQuerySystemInformation"
        );

    const auto queryProcess =
        ResolveNtdllFunction<NtQueryInformationProcessFn>(
            "NtQueryInformationProcess"
        );

    if (querySystem == nullptr || queryProcess == nullptr) {
        std::cerr << "Required ntdll functions were not found.\n";
        WaitForExit();
        return 1;
    }

    const DWORD processId = FindProcessId(querySystem);

    if (processId == 0) {
        std::cerr << "crossfire.exe was not found.\n";
        WaitForExit();
        return 2;
    }

    std::cout << "PID: " << processId << '\n';

    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        processId
    ));

    if (!process) {
        std::cerr
            << "OpenProcess failed, Win32="
            << GetLastError()
            << '\n';
        WaitForExit();
        return 3;
    }

    const std::uintptr_t imageBase =
        FindImageBase(process.get(), queryProcess);

    if (imageBase == 0) {
        std::cerr
            << "The module base address was not available.\n";
        WaitForExit();
        return 4;
    }

    std::cout
        << "Module base: 0x"
        << std::hex
        << std::uppercase
        << imageBase
        << std::dec
        << '\n';

    WaitForExit();
    return 0;
}
