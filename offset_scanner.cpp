#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kProcessName[] = L"crossfire.exe";
constexpr std::uintptr_t kMinimumAddress = 0x00400000ULL;
constexpr std::uintptr_t kMaximumAddress = 0x7FFFFFFFULL;
constexpr SIZE_T kChunkSize = 1024 * 1024;
constexpr float kMinimumCoordinate = -1000000.0f;
constexpr float kMaximumCoordinate = 1000000.0f;
constexpr float kMinimumDelta = 0.01f;
constexpr float kMaximumDelta = 100000.0f;
constexpr float kStableEpsilon = 0.001f;

struct MemoryChunk {
    std::uintptr_t address;
    std::vector<std::byte> data;
};

struct Candidate {
    std::uintptr_t address;
    float before;
    float after;
};

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const { return handle_; }
    explicit operator bool() const {
        return handle_ && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

DWORD FindProcessId() {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return 0;
    }

    do {
        if (_wcsicmp(entry.szExeFile, kProcessName) == 0) {
            return entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot.get(), &entry));

    return 0;
}

bool IsReadable(const MEMORY_BASIC_INFORMATION& information) {
    if (information.State != MEM_COMMIT) {
        return false;
    }

    if (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) {
        return false;
    }

    const DWORD protection = information.Protect & 0xFF;
    return protection == PAGE_READONLY ||
           protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool IsPlausibleFloat(float value) {
    return std::isfinite(value) &&
           value >= kMinimumCoordinate &&
           value <= kMaximumCoordinate;
}

std::vector<MemoryChunk> CaptureSnapshot(HANDLE process) {
    std::vector<MemoryChunk> snapshot;
    std::uintptr_t queryAddress = kMinimumAddress;
    std::uint64_t totalBytes = 0;

    while (queryAddress <= kMaximumAddress) {
        MEMORY_BASIC_INFORMATION information{};
        const SIZE_T queryResult = VirtualQueryEx(
            process,
            reinterpret_cast<LPCVOID>(queryAddress),
            &information,
            sizeof(information)
        );

        if (queryResult == 0) {
            queryAddress += 0x1000;
            continue;
        }

        const auto regionBase = reinterpret_cast<std::uintptr_t>(
            information.BaseAddress
        );
        const std::uintptr_t regionEnd = regionBase + information.RegionSize;

        if (IsReadable(information)) {
            std::uintptr_t chunkAddress = std::max(
                regionBase,
                kMinimumAddress
            );
            const std::uintptr_t readableEnd = std::min(
                regionEnd,
                kMaximumAddress + 1
            );
            chunkAddress = (chunkAddress + 3) & ~std::uintptr_t{3};

            while (chunkAddress + sizeof(float) <= readableEnd) {
                const SIZE_T requested = static_cast<SIZE_T>(std::min<
                    std::uintptr_t
                >(kChunkSize, readableEnd - chunkAddress));

                MemoryChunk chunk{};
                chunk.address = chunkAddress;
                chunk.data.resize(requested);

                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(
                        process,
                        reinterpret_cast<LPCVOID>(chunkAddress),
                        chunk.data.data(),
                        requested,
                        &bytesRead
                    ) && bytesRead >= sizeof(float)) {
                    bytesRead -= bytesRead % sizeof(float);
                    chunk.data.resize(bytesRead);
                    totalBytes += bytesRead;
                    snapshot.push_back(std::move(chunk));
                }

                chunkAddress += requested;
            }
        }

        const std::uintptr_t nextAddress = std::max(
            queryAddress + 0x1000,
            regionEnd
        );
        if (nextAddress <= queryAddress) {
            break;
        }
        queryAddress = nextAddress;
    }

    std::cout << "Captured "
              << std::fixed << std::setprecision(1)
              << static_cast<double>(totalBytes) / (1024.0 * 1024.0)
              << " MiB\n";
    return snapshot;
}

std::vector<Candidate> CompareSnapshot(
    HANDLE process,
    const std::vector<MemoryChunk>& snapshot
) {
    std::vector<Candidate> candidates;

    for (const MemoryChunk& chunk : snapshot) {
        std::vector<std::byte> current(chunk.data.size());
        SIZE_T bytesRead = 0;

        if (!ReadProcessMemory(
                process,
                reinterpret_cast<LPCVOID>(chunk.address),
                current.data(),
                current.size(),
                &bytesRead
            )) {
            continue;
        }

        bytesRead = std::min(bytesRead, chunk.data.size());
        bytesRead -= bytesRead % sizeof(float);

        for (SIZE_T offset = 0;
             offset + sizeof(float) <= bytesRead;
             offset += sizeof(float)) {
            float before = 0.0f;
            float after = 0.0f;
            std::memcpy(&before, chunk.data.data() + offset, sizeof(float));
            std::memcpy(&after, current.data() + offset, sizeof(float));

            if (!IsPlausibleFloat(before) || !IsPlausibleFloat(after)) {
                continue;
            }

            const float delta = std::abs(after - before);
            if (delta >= kMinimumDelta && delta <= kMaximumDelta) {
                candidates.push_back({
                    chunk.address + offset,
                    before,
                    after
                });
            }
        }
    }

    return candidates;
}

std::vector<Candidate> KeepStableCandidates(
    HANDLE process,
    const std::vector<Candidate>& candidates
) {
    std::vector<Candidate> stable;
    stable.reserve(candidates.size());

    Sleep(750);

    for (const Candidate& candidate : candidates) {
        float value = std::numeric_limits<float>::quiet_NaN();
        SIZE_T bytesRead = 0;

        if (ReadProcessMemory(
                process,
                reinterpret_cast<LPCVOID>(candidate.address),
                &value,
                sizeof(value),
                &bytesRead
            ) && bytesRead == sizeof(value) &&
            IsPlausibleFloat(value) &&
            std::abs(value - candidate.after) <= kStableEpsilon) {
            stable.push_back(candidate);
        }
    }

    return stable;
}

} 

int main() {
    const DWORD processId = FindProcessId();
    if (processId == 0) {
        std::cerr << "crossfire.exe was not found\n";
        return 1;
    }

    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        processId
    ));

    if (!process) {
        std::cerr << "OpenProcess failed, Win32="
                  << GetLastError() << '\n';
        return 2;
    }

    std::cout << "PID=" << processId << '\n';
    std::cout << "Keep the player still and press Enter...\n";
    std::cin.get();

    const std::vector<MemoryChunk> baseline = CaptureSnapshot(process.get());
    if (baseline.empty()) {
        std::cerr << "No readable memory was captured, Win32="
                  << GetLastError() << '\n';
        return 3;
    }

    std::cout << "Move the player, then stop and press Enter...\n";
    std::cin.get();

    std::vector<Candidate> candidates = CompareSnapshot(
        process.get(),
        baseline
    );

    std::cout << "Changed float candidates: "
              << candidates.size() << '\n';
    std::cout << "Keep the player still while candidates are verified...\n";

    candidates = KeepStableCandidates(process.get(), candidates);
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.address < right.address;
        }
    );

    for (const Candidate& candidate : candidates) {
        std::cout << "0x"
                  << std::hex << std::uppercase
                  << candidate.address
                  << std::dec << std::nouppercase
                  << " before=" << candidate.before
                  << " after=" << candidate.after
                  << " delta=" << std::abs(candidate.after - candidate.before)
                  << '\n';
    }

    std::cout << "Stable moving-float candidates: "
              << candidates.size() << '\n';
    return 0;
}
