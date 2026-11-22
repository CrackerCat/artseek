#include "mem.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MADV_COLD
#define MADV_COLD 20
#endif

namespace artseek {

std::optional<uint64_t> readProcStartTime(int pid) {
    if (pid <= 0) return std::nullopt;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    char buf[4096];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return std::nullopt;
    buf[n] = '\0';
    // Find last ')' because comm can contain spaces or ')'
    const char* rp = std::strrchr(buf, ')');
    if (!rp || rp[1] != ' ') return std::nullopt;
    const char* p = rp + 2;
    // Field 3 (state) is index 0 here; starttime is field 22 -> index 19
    int fieldIdx = 0;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\n') ++p;
        if (fieldIdx == 19) {
            std::string tok(start, static_cast<size_t>(p - start));
            char* endp = nullptr;
            unsigned long long v = std::strtoull(tok.c_str(), &endp, 10);
            if (endp && *endp == '\0') return static_cast<uint64_t>(v);
            return std::nullopt;
        }
        ++fieldIdx;
    }
    return std::nullopt;
}

std::optional<int> findPidByPackage(const std::string& pkg) {
    if (pkg.empty()) return std::nullopt;
    DIR* dir = ::opendir("/proc");
    if (!dir) return std::nullopt;
    int foundPid = -1;
    struct dirent* ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        int pid = std::atoi(ent->d_name);
        if (pid <= 0) continue;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char buf[512];
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        // argv[0] is NUL-terminated
        std::string procName(buf);
        if (procName == pkg) {
            // Verify not zombie
            if (readProcStartTime(pid).has_value()) {
                foundPid = pid;
                break;
            }
        }
    }
    ::closedir(dir);
    if (foundPid > 0) return foundPid;
    return std::nullopt;
}

bool isProcess64Bit(int pid, std::string* detail) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (detail) *detail = std::string("cannot open exe: ") + std::strerror(errno);
        return false;
    }
    uint8_t hdr[20] = {0};
    ssize_t n = ::read(fd, hdr, sizeof(hdr));
    ::close(fd);
    if (n < 5) {
        if (detail) *detail = "short read on exe header";
        return false;
    }
    if (hdr[4] == 2) {
        if (detail) *detail = "64-bit ELF (arm64/x86_64)";
        return true;
    }
    if (hdr[4] == 1) {
        if (detail) *detail = "32-bit ELF (unsupported)";
        return false;
    }
    if (detail) *detail = "unknown ELF EI_CLASS=" + std::to_string(hdr[4]);
    return false;
}

ProcMem::ProcMem() {
    void* p = ::mmap(nullptr, kScanBufSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        scanBuf_ = static_cast<uint8_t*>(p);
    }
}

ProcMem::~ProcMem() {
    close();
    if (scanBuf_) {
        ::munmap(scanBuf_, kScanBufSize);
        scanBuf_ = nullptr;
    }
}

bool ProcMem::open(int pid, std::string* err) {
    close();
    if (pid <= 0) {
        if (err) *err = "invalid pid";
        return false;
    }

    char pmPath[64];
    std::snprintf(pmPath, sizeof(pmPath), "/proc/%d/pagemap", pid);
    int pmFd = ::open(pmPath, O_RDONLY | O_CLOEXEC); // optional

    pid_ = pid;
    pagemapFd_ = pmFd;
    vmReadvUnsupported_ = false;
    memFdAttempted_ = false;

#ifndef SYS_process_vm_readv
    vmReadvUnsupported_ = true;
    if (ensureMemFd() < 0) {
        int savedErr = errno;
        if (err) {
            char memPath[64];
            std::snprintf(memPath, sizeof(memPath), "/proc/%d/mem", pid);
            *err = std::string("open ") + memPath + ": " + std::strerror(savedErr);
        }
        close();
        return false;
    }
#endif

    if (!refreshMaps(err)) {
        close();
        return false;
    }
    open_ = true;
    return true;
}

void ProcMem::close() {
    clearPageCache();
    if (memFd_ >= 0) { ::close(memFd_); memFd_ = -1; }
    if (pagemapFd_ >= 0) { ::close(pagemapFd_); pagemapFd_ = -1; }
    pid_ = -1;
    open_ = false;
    vmReadvUnsupported_ = false;
    memFdAttempted_ = false;
    allRegions_.clear();
    heapRegions_.clear();
    roFileRegions_.clear();
    mainBitmap_ = LiveBitmapInfo{};
}

int ProcMem::ensureMemFd() {
    if (memFd_ >= 0) return memFd_;
    if (memFdAttempted_ || pid_ <= 0) return -1;
    memFdAttempted_ = true;
    char memPath[64];
    std::snprintf(memPath, sizeof(memPath), "/proc/%d/mem", pid_);
    memFd_ = ::open(memPath, O_RDONLY | O_CLOEXEC);
    return memFd_;
}

static bool containsSub(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}

bool ProcMem::refreshMaps(std::string* err) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", pid_);
    std::ifstream f(path);
    if (!f.is_open()) {
        if (err) *err = std::string("open ") + path + ": " + std::strerror(errno);
        return false;
    }

    allRegions_.clear();
    heapRegions_.clear();
    roFileRegions_.clear();
    mainBitmap_ = LiveBitmapInfo{};

    static const char* kSkipSub[] = {
        "bitmap", "LinearAlloc", "allocation stack", "reservation",
        "indirect ref", "mod union", "mark stack", "card table", "read barrier",
        "large object", "large marked objects", "large live objects", "live stack",
        "CompilerMetadata", "local ref table", "concurrent copying sweep", "live objects",
        "Sentinel fault page", "DEX data", "jit-code-cache", "jit-data-cache",
        "jit-zygote-cache"
    };

    uint64_t mainStart = 0, mainEnd = 0;
    uint64_t bmStart = 0, bmEnd = 0;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // Format: start-end perm offset dev inode [pathname...]
        const char* s = line.c_str();
        char* end1 = nullptr;
        unsigned long long rStart = std::strtoull(s, &end1, 16);
        if (!end1 || *end1 != '-') continue;
        char* end2 = nullptr;
        unsigned long long rEnd = std::strtoull(end1 + 1, &end2, 16);
        if (!end2 || *end2 != ' ') continue;
        const char* p = end2;
        while (*p == ' ') ++p;
        const char* permStart = p;
        while (*p && *p != ' ') ++p;
        std::string perm(permStart, static_cast<size_t>(p - permStart));
        // Skip offset, dev, inode (3 fields)
        for (int skip = 0; skip < 3; ++skip) {
            while (*p == ' ') ++p;
            while (*p && *p != ' ') ++p;
        }
        while (*p == ' ') ++p;
        std::string name(p);
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();

        Region reg{static_cast<uint64_t>(rStart), static_cast<uint64_t>(rEnd), perm, name};
        if (perm.size() >= 1 && perm[0] == 'r') {
            allRegions_.push_back(reg);
            if (perm.rfind("r--", 0) == 0 && !name.empty() && name[0] == '/' &&
                (containsSub(name, ".dex") || containsSub(name, ".vdex") ||
                 containsSub(name, ".apk") || containsSub(name, ".jar") ||
                 containsSub(name, ".odex"))) {
                roFileRegions_.push_back(reg);
            }
        }

        if (perm.rfind("rw", 0) == 0) {
            if (name == "[anon:dalvik-main space (region space)]") {
                mainStart = reg.start;
                mainEnd = reg.end;
            } else if (name == "[anon:dalvik-region space live bitmap]") {
                bmStart = reg.start;
                bmEnd = reg.end;
            }
        }

        bool isDalvikAnon = (name.rfind("[anon:dalvik-", 0) == 0);
        bool isDalvikCache = containsSub(name, "dalvik-cache");
        if (!isDalvikAnon && !isDalvikCache) continue;
        if (perm.rfind("rw", 0) != 0 && !isDalvikCache) continue;
        if (perm.empty() || perm[0] != 'r') continue;

        bool skip = false;
        for (const char* sub : kSkipSub) {
            if (containsSub(name, sub)) {
                skip = true;
                break;
            }
        }
        if (skip) continue;

        // Clip .art boot/app image regions to kSectionObjects (excludes ArtFields, ArtMethods,
        // ImTables, DexCacheArrays, Metadata which are non-mirror::Object sections!).
        if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".art]") == 0 &&
            reg.end > reg.start + 128) {
            uint8_t hdr[128] = {0};
            if (readRaw(reg.start, hdr, sizeof(hdr)) == sizeof(hdr) &&
                std::memcmp(hdr, "art\n", 4) == 0) {
                uint32_t imgBegin = 0, imgSize = 0;
                std::memcpy(&imgBegin, hdr + 0x10, 4);
                std::memcpy(&imgSize, hdr + 0x14, 4);
                uint64_t mapLen = reg.end - reg.start;
                if (imgBegin == static_cast<uint32_t>(reg.start) &&
                    imgSize > 0 && imgSize <= mapLen) {
                    // Locate pointer_size_ (== 8) followed by sections_[0] (offset == 0, size == objSize)
                    // and sections_[1].offset >= objSize && <= objSize + 16
                    for (size_t off = 0x30; off + 16 <= sizeof(hdr); off += 4) {
                        uint32_t ptrSz = 0, s0Off = 0, s0Sz = 0, s1Off = 0;
                        std::memcpy(&ptrSz, hdr + off, 4);
                        std::memcpy(&s0Off, hdr + off + 4, 4);
                        std::memcpy(&s0Sz, hdr + off + 8, 4);
                        std::memcpy(&s1Off, hdr + off + 12, 4);
                        if (ptrSz == 8 && s0Off == 0 && s0Sz > 0 && s0Sz <= imgSize &&
                            s1Off >= s0Sz && s1Off <= s0Sz + 16) {
                            reg.end = reg.start + s0Sz;
                            break;
                        }
                    }
                }
            }
        }

        heapRegions_.push_back(std::move(reg));
    }

    auto cmpStart = [](const Region& a, const Region& b) { return a.start < b.start; };
    std::sort(allRegions_.begin(), allRegions_.end(), cmpStart);
    std::sort(heapRegions_.begin(), heapRegions_.end(), cmpStart);

    if (mainEnd > mainStart && bmEnd > bmStart &&
        (bmEnd - bmStart) * 64ULL == (mainEnd - mainStart)) {
        mainBitmap_.heapStart = mainStart;
        mainBitmap_.heapEnd = mainEnd;
        mainBitmap_.bmStart = bmStart;
        mainBitmap_.bmEnd = bmEnd;
        mainBitmap_.valid = true;
    }

    return true;
}

bool ProcMem::isMapped(uint64_t addr) const {
    if (addr < 0x1000) return false;
    auto it = std::upper_bound(allRegions_.begin(), allRegions_.end(), addr,
                               [](uint64_t a, const Region& r) { return a < r.start; });
    if (it == allRegions_.begin()) return false;
    --it;
    return addr >= it->start && addr < it->end;
}

bool ProcMem::isMappedRange(uint64_t addr, size_t len) const {
    if (addr < 0x1000 || len == 0) return false;
    if (addr + len < addr) return false;
    return isMapped(addr) && isMapped(addr + len - 1);
}

size_t ProcMem::readRaw(uint64_t addr, void* dst, size_t len) {
    if (len == 0 || pid_ <= 0) return 0;
#ifdef SYS_process_vm_readv
    if (!vmReadvUnsupported_) {
        struct iovec local_iov{dst, len};
        struct iovec remote_iov{reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), len};
        ssize_t n = ::syscall(SYS_process_vm_readv, pid_, &local_iov, 1UL, &remote_iov, 1UL, 0UL);
        if (n > 0) return static_cast<size_t>(n);
        if (n == 0) return 0;
        int e = errno;
        // Normal address-level errors (e.g. EFAULT on unmapped/guard pages, ESRCH, EINVAL, ENOMEM)
        // must NOT trigger opening /proc/<pid>/mem, which can trip inotify anti-dump watches.
        if (e != ENOSYS && e != EPERM && e != EACCES) {
            return 0;
        }
        vmReadvUnsupported_ = true;
    }
#endif
    int fd = ensureMemFd();
    if (fd >= 0) {
        ssize_t n2 = ::pread64(fd, dst, len, static_cast<off64_t>(addr));
        if (n2 > 0) return static_cast<size_t>(n2);
    }
    return 0;
}

const uint8_t* ProcMem::getPage(uint64_t pageBase) {
    auto it = pages_.find(pageBase);
    if (it != pages_.end()) {
        return it->second.data();
    }
    if (pageOrder_.size() >= kMaxCachedPages) {
        pages_.erase(pageOrder_.front());
        pageOrder_.pop_front();
    }
    auto [insIt, _] = pages_.emplace(pageBase, std::array<uint8_t, kPageSize>{});
    uint8_t* buf = insIt->second.data();
    size_t got = readRaw(pageBase, buf, kPageSize);
    ++pageReads_;
    if (got < kPageSize) {
        std::memset(buf + got, 0, kPageSize - got);
    }
    pageOrder_.push_back(pageBase);
    return buf;
}

bool ProcMem::readBytes(uint64_t addr, void* dst, size_t len) {
    if (len == 0) return true;
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t rem = len;
    uint64_t cur = addr;
    while (rem > 0) {
        uint64_t pageBase = cur & ~(static_cast<uint64_t>(kPageSize) - 1);
        size_t off = static_cast<size_t>(cur - pageBase);
        size_t avail = kPageSize - off;
        if (avail > rem) avail = rem;
        const uint8_t* pg = getPage(pageBase);
        std::memcpy(out, pg + off, avail);
        out += avail;
        cur += avail;
        rem -= avail;
    }
    return true;
}

std::vector<uint8_t> ProcMem::bytes(uint64_t addr, size_t len) {
    std::vector<uint8_t> out(len);
    if (len > 0) readBytes(addr, out.data(), len);
    return out;
}

uint8_t ProcMem::u8(uint64_t addr) {
    uint8_t v = 0;
    readBytes(addr, &v, 1);
    return v;
}

uint16_t ProcMem::u16(uint64_t addr) {
    uint16_t v = 0;
    readBytes(addr, &v, 2);
    return v;
}

uint32_t ProcMem::u32(uint64_t addr) {
    uint32_t v = 0;
    readBytes(addr, &v, 4);
    return v;
}

int32_t ProcMem::i32(uint64_t addr) {
    return static_cast<int32_t>(u32(addr));
}

uint64_t ProcMem::u64(uint64_t addr) {
    uint64_t v = 0;
    readBytes(addr, &v, 8);
    return v;
}

int64_t ProcMem::i64(uint64_t addr) {
    return static_cast<int64_t>(u64(addr));
}

float ProcMem::f32(uint64_t addr) {
    uint32_t bits = u32(addr);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

double ProcMem::f64(uint64_t addr) {
    uint64_t bits = u64(addr);
    double d = 0.0;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

bool ProcMem::readPagemapEntries(uint64_t startAddr, size_t pageCount, std::vector<uint64_t>& out) {
    if (pagemapFd_ < 0 || pageCount == 0) return false;
    out.resize(pageCount);
    uint64_t fileOff = (startAddr / kPageSize) * sizeof(uint64_t);
    size_t wantBytes = pageCount * sizeof(uint64_t);
    ssize_t got = ::pread64(pagemapFd_, out.data(), wantBytes, static_cast<off64_t>(fileOff));
    if (got != static_cast<ssize_t>(wantBytes)) {
        return false;
    }
    return true;
}

void ProcMem::scanRegionViaPagemap(
    const Region& r,
    const std::function<bool(uint64_t base, const uint8_t* buf, size_t len)>& fn) {
    if (r.end <= r.start) return;
    std::vector<uint8_t> fallbackBuf;
    uint8_t* buf = scanBuf_;
    if (!buf) {
        fallbackBuf.resize(kScanBufSize);
        buf = fallbackBuf.data();
    }

    uint64_t totalPages = (r.end - r.start) / kPageSize;
    constexpr size_t kBatchPages = 65536; // 256MB of VA -> 512KB pagemap read
    std::vector<uint64_t> pm;

    // Only pay pagemap lookup when region > 512KB (tiny regions read directly in 1 call)
    bool usePagemap = (pagemapFd_ >= 0 && totalPages >= 128);
    if (!usePagemap) {
        for (uint64_t a = r.start; a + 8 <= r.end; ) {
            size_t chunk = static_cast<size_t>(std::min<uint64_t>(kScanBufSize, r.end - a));
            size_t got = readRaw(a, buf, chunk);
            if (got >= 8) {
                if (!fn(a, buf, got)) return;
            }
            a += chunk;
        }
        return;
    }

    uint64_t curPage = 0;
    while (curPage < totalPages) {
        size_t batch = static_cast<size_t>(std::min<uint64_t>(kBatchPages, totalPages - curPage));
        uint64_t batchStart = r.start + curPage * kPageSize;
        if (!readPagemapEntries(batchStart, batch, pm)) {
            // Fallback to full chunk scan for remainder of region
            for (uint64_t a = batchStart; a + 8 <= r.end; ) {
                size_t chunk = static_cast<size_t>(std::min<uint64_t>(kScanBufSize, r.end - a));
                size_t got = readRaw(a, buf, chunk);
                if (got >= 8) {
                    if (!fn(a, buf, got)) return;
                }
                a += chunk;
            }
            return;
        }

        // Coalesce contiguous present or swapped pages into runs up to kScanBufSize
        size_t i = 0;
        constexpr uint64_t kPresentOrSwapped = (1ULL << 63) | (1ULL << 62);
        while (i < batch) {
            if ((pm[i] & kPresentOrSwapped) == 0) {
                ++i;
                continue;
            }
            size_t runStart = i;
            size_t maxRunPages = kScanBufSize / kPageSize;
            while (i < batch && (i - runStart) < maxRunPages && (pm[i] & kPresentOrSwapped) != 0) {
                ++i;
            }
            uint64_t addr = batchStart + static_cast<uint64_t>(runStart) * kPageSize;
            size_t len = (i - runStart) * kPageSize;
            if (addr + len > r.end) len = static_cast<size_t>(r.end - addr);
            size_t got = readRaw(addr, buf, len);
            if (got >= 8) {
                if (!fn(addr, buf, got)) return;
            }
        }
        curPage += batch;
    }
}

void ProcMem::eachHeapChunk(
    const std::function<bool(uint64_t base, const uint8_t* buf, size_t len)>& fn) {
    bool stop = false;
    auto wrapper = [&](uint64_t base, const uint8_t* buf, size_t len) -> bool {
        if (!fn(base, buf, len)) {
            stop = true;
            return false;
        }
        return true;
    };
    for (const auto& r : heapRegions_) {
        if (stop) break;
        scanRegionViaPagemap(r, wrapper);
    }
}

void ProcMem::eachHeapWord8(
    const std::function<bool(uint64_t addr, uint32_t word0)>& fn,
    bool useBitmap) {
    bool stop = false;
    for (const auto& r : heapRegions_) {
        if (stop) break;
        // For main space with valid live bitmap:
        // ART RegionSpace uses 256KB regions -> 1 page (4KB) of live bitmap per 256KB region.
        // Note: ToSpace/TLAB bump-pointer regions can have all-zero bitmap pages while still holding
        // newly allocated live objects! We check each 256KB region's bitmap page:
        // - If heap region has zero resident pages -> skip completely.
        // - If bitmap page is resident & non-zero -> scan using set bits (exact live objects!).
        // - If bitmap page is absent or all-zero -> fall back to pagemap-filtered 8-byte scan
        //   of that 256KB region so newly allocated ToSpace objects are NEVER missed!
        if (useBitmap && mainBitmap_.valid &&
            r.start == mainBitmap_.heapStart && r.end == mainBitmap_.heapEnd) {
            constexpr uint64_t kArtRegionSize = 256 * 1024; // 256 KB
            constexpr size_t kPagesPerArtRegion = kArtRegionSize / kPageSize; // 64 pages
            constexpr uint64_t kPresentOrSwapped = (1ULL << 63) | (1ULL << 62);

            uint64_t numArtRegions = (r.end - r.start) / kArtRegionSize;
            std::vector<uint64_t> bmPm;
            std::vector<uint64_t> heapPm;
            bool pmOk = (pagemapFd_ >= 0) &&
                        readPagemapEntries(mainBitmap_.bmStart, static_cast<size_t>(numArtRegions), bmPm);
            if (!pmOk) {
                // Fallback to regular chunk scan
                scanRegionViaPagemap(r, [&](uint64_t base, const uint8_t* buf, size_t len) -> bool {
                    for (size_t off = 0; off + 8 <= len; off += 8) {
                        uint32_t w0;
                        std::memcpy(&w0, buf + off, 4);
                        if (w0 >= 0x1000 && !fn(base + off, w0)) {
                            stop = true;
                            return false;
                        }
                    }
                    return true;
                });
                continue;
            }

            std::array<uint8_t, kPageSize> bmPage{};
            std::vector<uint8_t> regBuf(kArtRegionSize);

            for (uint64_t regIdx = 0; regIdx < numArtRegions && !stop; ++regIdx) {
                uint64_t regAddr = r.start + regIdx * kArtRegionSize;
                if (!readPagemapEntries(regAddr, kPagesPerArtRegion, heapPm)) {
                    // Fallback single region read
                    size_t got = readRaw(regAddr, regBuf.data(), kArtRegionSize);
                    for (size_t off = 0; off + 8 <= got; off += 8) {
                        uint32_t w0;
                        std::memcpy(&w0, regBuf.data() + off, 4);
                        if (w0 >= 0x1000 && !fn(regAddr + off, w0)) {
                            stop = true;
                            break;
                        }
                    }
                    continue;
                }
                // Check if any page in this 256KB region is resident/swapped
                size_t firstRes = kPagesPerArtRegion;
                size_t lastRes = 0;
                for (size_t p = 0; p < kPagesPerArtRegion; ++p) {
                    if (heapPm[p] & kPresentOrSwapped) {
                        if (firstRes == kPagesPerArtRegion) firstRes = p;
                        lastRes = p + 1;
                    }
                }
                if (firstRes == kPagesPerArtRegion) continue; // completely unpopulated region!

                bool bmHasBits = false;
                if (bmPm[regIdx] & kPresentOrSwapped) {
                    uint64_t bmAddr = mainBitmap_.bmStart + regIdx * kPageSize;
                    if (readRaw(bmAddr, bmPage.data(), kPageSize) == kPageSize) {
                        const uint64_t* words = reinterpret_cast<const uint64_t*>(bmPage.data());
                        for (size_t wi = 0; wi < kPageSize / 8; ++wi) {
                            if (words[wi] != 0) {
                                bmHasBits = true;
                                break;
                            }
                        }
                    }
                }

                // Read only the resident span [firstRes, lastRes)
                uint64_t spanAddr = regAddr + firstRes * kPageSize;
                size_t spanLen = (lastRes - firstRes) * kPageSize;
                size_t got = readRaw(spanAddr, regBuf.data() + firstRes * kPageSize, spanLen);
                if (got < 8) continue;
                size_t validEnd = firstRes * kPageSize + got;

                if (bmHasBits) {
                    // Use live bitmap: 1 bit per 8 bytes -> 64 bits = 512 bytes of heap
                    const uint64_t* words = reinterpret_cast<const uint64_t*>(bmPage.data());
                    for (size_t wi = 0; wi < kPageSize / 8 && !stop; ++wi) {
                        uint64_t bits = words[wi];
                        while (bits) {
                            unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
                            bits &= (bits - 1);
                            size_t off = (wi * 64 + bit) * 8;
                            if (off < firstRes * kPageSize || off + 8 > validEnd) continue;
                            uint32_t w0;
                            std::memcpy(&w0, regBuf.data() + off, 4);
                            if (w0 >= 0x1000 && !fn(regAddr + off, w0)) {
                                stop = true;
                                break;
                            }
                        }
                    }
                } else {
                    // ToSpace / TLAB region: scan resident pages at 8-byte stride
                    for (size_t p = firstRes; p < lastRes && !stop; ++p) {
                        if ((heapPm[p] & kPresentOrSwapped) == 0) continue;
                        size_t pStart = p * kPageSize;
                        size_t pEnd = std::min(pStart + kPageSize, validEnd);
                        for (size_t off = pStart; off + 8 <= pEnd; off += 8) {
                            uint32_t w0;
                            std::memcpy(&w0, regBuf.data() + off, 4);
                            if (w0 >= 0x1000 && !fn(regAddr + off, w0)) {
                                stop = true;
                                break;
                            }
                        }
                    }
                }
            }
            continue;
        }

        scanRegionViaPagemap(r, [&](uint64_t base, const uint8_t* buf, size_t len) -> bool {
            for (size_t off = 0; off + 8 <= len; off += 8) {
                uint32_t w0;
                std::memcpy(&w0, buf + off, 4);
                if (w0 >= 0x1000 && !fn(base + off, w0)) {
                    stop = true;
                    return false;
                }
            }
            return true;
        });
    }
}

void ProcMem::clearPageCache() {
    pages_.clear();
    pageOrder_.clear();
}

void ProcMem::madviseCleanup(bool enableMadvise) {
    clearPageCache();
    if (!enableMadvise) return;

    // Release our own scratch scan buffer pages back to kernel
    if (scanBuf_) {
        ::madvise(scanBuf_, kScanBufSize, MADV_DONTNEED);
    }

    // Best-effort process_madvise(MADV_COLD) ONLY on clean read-only file-backed mappings
    // (NEVER MADV_DONTNEED on target's anonymous heap which would zero live data!)
#if defined(SYS_pidfd_open) && defined(SYS_process_madvise)
    if (pid_ > 0 && !roFileRegions_.empty()) {
        int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid_, 0U));
        if (pidfd >= 0) {
            constexpr size_t kMaxIov = 64;
            struct iovec iovs[kMaxIov];
            size_t cnt = 0;
            for (const auto& r : roFileRegions_) {
                if (r.end <= r.start) continue;
                iovs[cnt].iov_base = reinterpret_cast<void*>(static_cast<uintptr_t>(r.start));
                iovs[cnt].iov_len = static_cast<size_t>(r.end - r.start);
                ++cnt;
                if (cnt == kMaxIov) {
                    ::syscall(SYS_process_madvise, pidfd, iovs, cnt, MADV_COLD, 0U);
                    cnt = 0;
                }
            }
            if (cnt > 0) {
                ::syscall(SYS_process_madvise, pidfd, iovs, cnt, MADV_COLD, 0U);
            }
            ::close(pidfd);
        }
    }
#endif
}

} // namespace artseek
