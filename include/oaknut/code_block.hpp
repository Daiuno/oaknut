// SPDX-FileCopyrightText: Copyright (c) 2022 merryhime <https://mary.rs>
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <cerrno>

#if defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#elif defined(__APPLE__)
#    include <TargetConditionals.h>
#    include <libkern/OSCacheControl.h>
#    include <pthread.h>
#    include <sys/mman.h>
#    include <sys/sysctl.h>
#    include <unistd.h>
#    include <mach/mach.h>
#    include <mach/vm_map.h>
#    include <dirent.h>
#    include <cstring>

#ifndef MAP_MEM_NAMED_CREATE
#    define MAP_MEM_NAMED_CREATE 0x020000
#endif
#ifndef MAP_MEM_LEDGER_TAGGED
#    define MAP_MEM_LEDGER_TAGGED 0x002000
#endif
#ifndef VM_LEDGER_TAG_DEFAULT
#    define VM_LEDGER_TAG_DEFAULT 0x00000001
#endif
#ifndef VM_LEDGER_FLAG_NO_FOOTPRINT
#    define VM_LEDGER_FLAG_NO_FOOTPRINT 0x00000001
#endif

#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(IOS)
#    include <mutex>
#    include <CoreFoundation/CoreFoundation.h>
#    include <IOKit/IOKitLib.h>
#endif

extern "C" {
    kern_return_t mach_memory_entry_ownership(
        mem_entry_name_port_t mem_entry,
        mach_port_t owner,
        int ledger_tag,
        int ledger_flags) __attribute__((weak_import));
}
#else
#    include <sys/mman.h>
#endif

namespace oaknut {

#if defined(__APPLE__) && defined(__arm64__)
#  if TARGET_OS_IPHONE || defined(IOS)
#    define OAKNUT_IOS_ARM64 1
#  endif
#endif

#ifdef OAKNUT_IOS_ARM64
enum class CodeBlockTXMProbe {
    Unknown,
    Yes,
    No,
};

inline int CodeBlockGetIOSMajorVersion() {
    char version_str[256] = {0};
    size_t size = sizeof(version_str);
    if (sysctlbyname("kern.osproductversion", version_str, &size, nullptr, 0) == 0) {
        return std::atoi(version_str);
    }
    return 0;
}

inline bool CodeBlockIsIOS26OrLater() {
    static bool checked = false;
    static bool is_ios26 = false;
    if (checked) {
        return is_ios26;
    }
    checked = true;

#if __has_builtin(__builtin_available)
    if (__builtin_available(iOS 26, *)) {
        is_ios26 = true;
        return true;
    }
#endif
    is_ios26 = CodeBlockGetIOSMajorVersion() >= 26;
    return is_ios26;
}

inline bool CodeBlockFindPathWithLength(const char* base_path, size_t target_length, char* out_path, size_t out_size) {
    DIR* dir = opendir(base_path);
    if (!dir) {
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strlen(entry->d_name) == target_length) {
            snprintf(out_path, out_size, "%s/%s", base_path, entry->d_name);
            closedir(dir);
            return true;
        }
    }
    closedir(dir);
    return false;
}

// iOS 26.6+: IODeviceTree:/chosen/memory-map lists a "TXM" key.
// Yes/No are conclusive. Unknown means IOKit could not answer — use firmware.
inline CodeBlockTXMProbe CodeBlockDeviceProbeTXMIOKit() {
    io_registry_entry_t memory_map =
        IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/chosen/memory-map");
    if (memory_map == IO_OBJECT_NULL) {
        return CodeBlockTXMProbe::Unknown;
    }

    CFTypeRef keys_ref = IORegistryEntryCreateCFProperty(
        memory_map, CFSTR("IORegistryEntryPropertyKeys"), kCFAllocatorDefault, 0);
    IOObjectRelease(memory_map);
    if (!keys_ref) {
        return CodeBlockTXMProbe::Unknown;
    }

    // Create returns +1; CFRelease once. Do not mix with takeUnretainedValue.
    CodeBlockTXMProbe result = CodeBlockTXMProbe::Unknown;
    if (CFGetTypeID(keys_ref) == CFArrayGetTypeID()) {
        result = CodeBlockTXMProbe::No;
        const CFArrayRef keys = static_cast<CFArrayRef>(keys_ref);
        const CFIndex count = CFArrayGetCount(keys);
        for (CFIndex i = 0; i < count; ++i) {
            const CFTypeRef item = CFArrayGetValueAtIndex(keys, i);
            if (item && CFGetTypeID(item) == CFStringGetTypeID() &&
                CFStringCompare(static_cast<CFStringRef>(item), CFSTR("TXM"), 0) ==
                    kCFCompareEqualTo) {
                result = CodeBlockTXMProbe::Yes;
                break;
            }
        }
    }
    CFRelease(keys_ref);
    return result;
}

// iOS 26.0-26.5: firmware file still exists on TXM devices.
inline bool CodeBlockDeviceHasTXMFirmware() {
    char boot_uuid_path[512];
    if (CodeBlockFindPathWithLength("/System/Volumes/Preboot", 36, boot_uuid_path, sizeof(boot_uuid_path))) {
        char boot_dir[512];
        snprintf(boot_dir, sizeof(boot_dir), "%s/boot", boot_uuid_path);

        char ninety_six_path[512];
        if (CodeBlockFindPathWithLength(boot_dir, 96, ninety_six_path, sizeof(ninety_six_path))) {
            char txm_path[1024];
            snprintf(txm_path, sizeof(txm_path),
                     "%s/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4",
                     ninety_six_path);
            if (access(txm_path, F_OK) == 0) {
                return true;
            }
        }
    }

    char fallback_path[512];
    if (CodeBlockFindPathWithLength("/private/preboot", 96, fallback_path, sizeof(fallback_path))) {
        char txm_path[1024];
        snprintf(txm_path, sizeof(txm_path),
                 "%s/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4",
                 fallback_path);
        if (access(txm_path, F_OK) == 0) {
            return true;
        }
    }
    return false;
}

inline bool CodeBlockDeviceHasTXM() {
    static bool checked = false;
    static bool has_txm = false;
    if (checked) {
        return has_txm;
    }
    checked = true;
    switch (CodeBlockDeviceProbeTXMIOKit()) {
    case CodeBlockTXMProbe::Yes:
        has_txm = true;
        break;
    case CodeBlockTXMProbe::No:
        has_txm = false;
        break;
    case CodeBlockTXMProbe::Unknown:
        has_txm = CodeBlockDeviceHasTXMFirmware();
        break;
    }
    return has_txm;
}

inline std::size_t CodeBlockGetPageSize() {
    static std::size_t page_size = 0;
    if (page_size == 0) {
        page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        if (page_size == 0) {
            page_size = 16384;
        }
    }
    return page_size;
}

inline std::size_t CodeBlockAlignToPage(std::size_t size) {
    const std::size_t page_size = CodeBlockGetPageSize();
    return (size + page_size - 1) & ~(page_size - 1);
}

// One process-wide dual-mapped arena. TXM can only brk/prepare once, then we suballocate.
struct IosJitArena {
    std::uint32_t* rx = nullptr;
    std::uint32_t* rw = nullptr;
    std::size_t size = 0;
    std::size_t used = 0;
    int refs = 0;
    bool is_txm = false;
};

inline IosJitArena& IosJitArenaInstance() {
    static IosJitArena arena;
    return arena;
}

inline std::mutex& IosJitArenaMutex() {
    static std::mutex mutex;
    return mutex;
}

constexpr std::size_t kIosJitArenaSize = 128 * 1024 * 1024;

inline void IosJitNotifyStikDebug(vm_address_t rx_addr, std::size_t actual_size) {
    const char* skip_brk = getenv("AZAHAR_SKIP_BRK");
    if (skip_brk && std::atoi(skip_brk) != 0) {
        return;
    }

    // CMD_PREPARE_REGION then CMD_DETACH must be back-to-back so other threads
    // cannot raise exceptions while StikDebug is still attached.
    __asm__ volatile(
        "mov x0, %0\n"
        "mov x1, %1\n"
        "mov x16, #1\n"
        "brk #0xf00d\n"
        "mov x0, #0\n"
        "mov x1, #0\n"
        "mov x16, #0\n"
        "brk #0xf00d"
        :
        : "r"(rx_addr), "r"(actual_size)
        : "x0", "x1", "x16");
}

inline void IosJitCreateTxmArena(IosJitArena& arena, std::size_t aligned_size) {
    memory_object_size_t memory_size = aligned_size;
    mach_port_t memory_entry = MACH_PORT_NULL;

    kern_return_t ret = mach_make_memory_entry_64(
        mach_task_self(),
        &memory_size,
        0,
        MAP_MEM_NAMED_CREATE | MAP_MEM_LEDGER_TAGGED |
            VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE,
        &memory_entry,
        MACH_PORT_NULL);
    if (ret != KERN_SUCCESS || memory_size < aligned_size) {
        if (memory_entry != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), memory_entry);
        }
        throw std::bad_alloc{};
    }

    const std::size_t actual_size = static_cast<std::size_t>(memory_size);
    if (mach_memory_entry_ownership != nullptr) {
        mach_memory_entry_ownership(memory_entry, MACH_PORT_NULL, VM_LEDGER_TAG_DEFAULT,
                                    VM_LEDGER_FLAG_NO_FOOTPRINT);
    }

    vm_address_t rx_addr = 0;
    ret = vm_map(mach_task_self(), &rx_addr, actual_size, 0, VM_FLAGS_ANYWHERE, memory_entry, 0,
                 FALSE, VM_PROT_READ | VM_PROT_EXECUTE,
                 VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE, VM_INHERIT_COPY);
    mach_port_deallocate(mach_task_self(), memory_entry);
    if (ret != KERN_SUCCESS) {
        throw std::bad_alloc{};
    }

    IosJitNotifyStikDebug(rx_addr, actual_size);

    vm_address_t rw_addr = 0;
    vm_prot_t cur_prot = 0;
    vm_prot_t max_prot = 0;
    ret = vm_remap(mach_task_self(), &rw_addr, actual_size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                   rx_addr, FALSE, &cur_prot, &max_prot, VM_INHERIT_NONE);
    if (ret != KERN_SUCCESS) {
        vm_deallocate(mach_task_self(), rx_addr, actual_size);
        throw std::bad_alloc{};
    }

    if (mprotect(reinterpret_cast<void*>(rw_addr), actual_size, PROT_READ | PROT_WRITE) != 0) {
        vm_deallocate(mach_task_self(), rw_addr, actual_size);
        vm_deallocate(mach_task_self(), rx_addr, actual_size);
        throw std::bad_alloc{};
    }

    arena.rx = reinterpret_cast<std::uint32_t*>(rx_addr);
    arena.rw = reinterpret_cast<std::uint32_t*>(rw_addr);
    arena.size = actual_size;
    arena.is_txm = true;
}

inline void IosJitCreatePplArena(IosJitArena& arena, std::size_t aligned_size) {
    void* rx_ptr = mmap(nullptr, aligned_size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (rx_ptr == MAP_FAILED) {
        throw std::bad_alloc{};
    }

    vm_address_t rw_addr = 0;
    vm_prot_t cur_prot = 0;
    vm_prot_t max_prot = 0;
    kern_return_t ret = vm_remap(mach_task_self(), &rw_addr, aligned_size, 0, VM_FLAGS_ANYWHERE,
                                 mach_task_self(), reinterpret_cast<vm_address_t>(rx_ptr), FALSE,
                                 &cur_prot, &max_prot, VM_INHERIT_NONE);
    if (ret != KERN_SUCCESS) {
        munmap(rx_ptr, aligned_size);
        throw std::bad_alloc{};
    }

    if (mprotect(reinterpret_cast<void*>(rw_addr), aligned_size, PROT_READ | PROT_WRITE) != 0) {
        vm_deallocate(mach_task_self(), rw_addr, aligned_size);
        munmap(rx_ptr, aligned_size);
        throw std::bad_alloc{};
    }

    arena.rx = reinterpret_cast<std::uint32_t*>(rx_ptr);
    arena.rw = reinterpret_cast<std::uint32_t*>(rw_addr);
    arena.size = aligned_size;
    arena.is_txm = false;
}

inline void IosJitEnsureArena() {
    IosJitArena& arena = IosJitArenaInstance();
    if (arena.rx) {
        return;
    }

    const std::size_t aligned_size = CodeBlockAlignToPage(kIosJitArenaSize);
    if (CodeBlockDeviceHasTXM()) {
        IosJitCreateTxmArena(arena, aligned_size);
    } else {
        IosJitCreatePplArena(arena, aligned_size);
    }
}

#endif // OAKNUT_IOS_ARM64

class CodeBlock {
public:
    explicit CodeBlock(std::size_t size)
        : m_size(size)
    {
#if defined(_WIN32)
        m_memory = (std::uint32_t*)VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#elif defined(__APPLE__)

#ifdef OAKNUT_IOS_ARM64
        if (CodeBlockIsIOS26OrLater()) {
            std::lock_guard<std::mutex> lock(IosJitArenaMutex());
            IosJitEnsureArena();

            IosJitArena& arena = IosJitArenaInstance();
            const std::size_t aligned_size = CodeBlockAlignToPage(size);
            if (arena.used + aligned_size > arena.size) {
                throw std::bad_alloc{};
            }

            const std::size_t offset = arena.used;
            arena.used += aligned_size;
            arena.refs += 1;

            m_memory = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(arena.rx) + offset);
            m_rw_memory = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(arena.rw) + offset);
            m_size = aligned_size;
            m_is_txm = arena.is_txm;
            m_is_no_txm = !arena.is_txm;
            return;
        }

        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (m_memory == MAP_FAILED) {
            m_memory = nullptr;
            throw std::bad_alloc{};
        }
#else
#    if TARGET_OS_IPHONE
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
#    else
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
#    endif
#endif // OAKNUT_IOS_ARM64

#elif defined(__NetBSD__)
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_MPROTECT(PROT_READ | PROT_WRITE | PROT_EXEC), MAP_ANON | MAP_PRIVATE, -1, 0);
#elif defined(__OpenBSD__)
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
#else
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
#endif

        if (m_memory == nullptr)
            throw std::bad_alloc{};
    }

    ~CodeBlock()
    {
        if (m_memory == nullptr)
            return;

#if defined(_WIN32)
        VirtualFree((void*)m_memory, 0, MEM_RELEASE);
#elif defined(__APPLE__) && defined(OAKNUT_IOS_ARM64)
        if (m_is_txm || m_is_no_txm) {
            // Keep the arena mapping for the process lifetime; TXM cannot re-prepare after detach.
            std::lock_guard<std::mutex> lock(IosJitArenaMutex());
            IosJitArena& arena = IosJitArenaInstance();
            if (arena.refs > 0) {
                arena.refs -= 1;
            }
            if (arena.refs == 0) {
                arena.used = 0;
            }
            return;
        }
        munmap(m_memory, m_size);
#else
        munmap(m_memory, m_size);
#endif
    }

    CodeBlock(const CodeBlock&) = delete;
    CodeBlock& operator=(const CodeBlock&) = delete;
    CodeBlock(CodeBlock&&) = delete;
    CodeBlock& operator=(CodeBlock&&) = delete;

    std::uint32_t* ptr() const
    {
        return m_memory;
    }

    std::uint32_t* wptr() const
    {
#ifdef OAKNUT_IOS_ARM64
        if (m_rw_memory) {
            return m_rw_memory;
        }
#endif
        return m_memory;
    }

    bool is_dual_mapping() const
    {
#ifdef OAKNUT_IOS_ARM64
        return m_is_txm || m_is_no_txm;
#else
        return false;
#endif
    }

    void protect()
    {
#if defined(__APPLE__) && !defined(OAKNUT_IOS_ARM64)
        pthread_jit_write_protect_np(1);
#elif defined(OAKNUT_IOS_ARM64)
        if (m_is_txm || m_is_no_txm) {
            return;
        }
        mprotect(m_memory, m_size, PROT_READ | PROT_EXEC);
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
        mprotect(m_memory, m_size, PROT_READ | PROT_EXEC);
#endif
    }

    void unprotect()
    {
#if defined(__APPLE__) && !defined(OAKNUT_IOS_ARM64)
        pthread_jit_write_protect_np(0);
#elif defined(OAKNUT_IOS_ARM64)
        if (m_is_txm || m_is_no_txm) {
            return;
        }
        mprotect(m_memory, m_size, PROT_READ | PROT_WRITE);
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
        mprotect(m_memory, m_size, PROT_READ | PROT_WRITE);
#endif
    }

    void invalidate(std::uint32_t* mem, std::size_t size)
    {
#if defined(__APPLE__)
#ifdef OAKNUT_IOS_ARM64
        if (m_is_txm || m_is_no_txm) {
            std::uint8_t* mem_byte = reinterpret_cast<std::uint8_t*>(mem);
            std::uint8_t* rx_base = reinterpret_cast<std::uint8_t*>(m_memory);
            std::uint8_t* rw_base = reinterpret_cast<std::uint8_t*>(m_rw_memory);

            std::uint32_t* invalidate_ptr = mem;
            if (m_rw_memory && mem_byte >= rw_base && mem_byte < rw_base + m_size) {
                invalidate_ptr = reinterpret_cast<std::uint32_t*>(rx_base + (mem_byte - rw_base));
            }

            // After TXM detach, sys_cache_control is the safe icache flush.
            if (m_is_txm) {
                sys_cache_control(kCacheFunctionPrepareForExecution, invalidate_ptr, size);
            } else {
                sys_icache_invalidate(invalidate_ptr, size);
            }
            return;
        }
#endif
        sys_icache_invalidate(mem, size);
#elif defined(_WIN32)
        FlushInstructionCache(GetCurrentProcess(), mem, size);
#else
        static std::size_t icache_line_size = 0x10000, dcache_line_size = 0x10000;

        std::uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0"
                         : "=r"(ctr));

        const std::size_t isize = icache_line_size = std::min<std::size_t>(icache_line_size, 4 << ((ctr >> 0) & 0xf));
        const std::size_t dsize = dcache_line_size = std::min<std::size_t>(dcache_line_size, 4 << ((ctr >> 16) & 0xf));

        const std::uintptr_t end = (std::uintptr_t)mem + size;

        for (std::uintptr_t addr = ((std::uintptr_t)mem) & ~(dsize - 1); addr < end; addr += dsize) {
            __asm__ volatile("dc cvau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
        __asm__ volatile("dsb ish\n"
                         :
                         :
                         : "memory");

        for (std::uintptr_t addr = ((std::uintptr_t)mem) & ~(isize - 1); addr < end; addr += isize) {
            __asm__ volatile("ic ivau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
        __asm__ volatile("dsb ish\nisb\n"
                         :
                         :
                         : "memory");
#endif
    }

    void invalidate_all()
    {
        invalidate(m_memory, m_size);
    }

protected:
    std::uint32_t* m_memory = nullptr;
#ifdef OAKNUT_IOS_ARM64
    std::uint32_t* m_rw_memory = nullptr;
    bool m_is_txm = false;
    bool m_is_no_txm = false;
#endif
    std::size_t m_size = 0;
};

} // namespace oaknut
