#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace artseek {

inline uint64_t untag(uint64_t v) {
    return v & 0x00FFFFFFFFFFFFFFULL;
}

struct Region {
    uint64_t start = 0;
    uint64_t end = 0;
    std::string perm;
    std::string name;
};

struct LiveBitmapInfo {
    uint64_t heapStart = 0;
    uint64_t heapEnd = 0;
    uint64_t bmStart = 0;
    uint64_t bmEnd = 0;
    bool valid = false;
};

std::optional<uint64_t> readProcStartTime(int pid);
std::optional<int> findPidByPackage(const std::string& pkg);
bool isProcess64Bit(int pid, std::string* detail = nullptr);

class ProcMem {
public:
    ProcMem();
    ~ProcMem();

    ProcMem(const ProcMem&) = delete;
    ProcMem& operator=(const ProcMem&) = delete;

    bool open(int pid, std::string* err = nullptr);
    void close();
    bool isOpen() const { return open_; }
    int pid() const { return pid_; }

    bool refreshMaps(std::string* err = nullptr);
    const std::vector<Region>& allRegions() const { return allRegions_; }
    const std::vector<Region>& heapRegions() const { return heapRegions_; }
    const LiveBitmapInfo& mainSpaceBitmap() const { return mainBitmap_; }

    bool isMapped(uint64_t addr) const;
    bool isMappedRange(uint64_t addr, size_t len) const;

    // Raw uncached read (uses process_vm_readv with lazy /proc/<pid>/mem pread fallback)
    size_t readRaw(uint64_t addr, void* dst, size_t len);

    // Page-cached reads for random object/DEX traversal
    bool readBytes(uint64_t addr, void* dst, size_t len);
    std::vector<uint8_t> bytes(uint64_t addr, size_t len);
    uint8_t u8(uint64_t addr);
    uint16_t u16(uint64_t addr);
    uint32_t u32(uint64_t addr);
    int32_t i32(uint64_t addr);
    uint64_t u64(uint64_t addr);
    int64_t i64(uint64_t addr);
    float f32(uint64_t addr);
    double f64(uint64_t addr);

    // Heap chunk iteration (skips uncommitted zero pages via pagemap)
    // Callback returns true to continue, false to stop early.
    void eachHeapChunk(const std::function<bool(uint64_t base, const uint8_t* buf, size_t len)>& fn);

    // 8-byte aligned heap object header scan (uses live bitmap on main space when valid + pagemap)
    void eachHeapWord8(const std::function<bool(uint64_t addr, uint32_t word0)>& fn, bool useBitmap = true);

    void clearPageCache();
    void madviseCleanup(bool enableMadvise);

    // Stats for diagnostics
    uint64_t pageReads() const { return pageReads_; }
    void resetStats() { pageReads_ = 0; }

private:
    static constexpr size_t kPageSize = 4096;
    static constexpr size_t kMaxCachedPages = 32768;
    static constexpr size_t kScanBufSize = 1u << 20; // 1 MiB

    const uint8_t* getPage(uint64_t pageBase);
    void scanRegionViaPagemap(const Region& r,
                              const std::function<bool(uint64_t base, const uint8_t* buf, size_t len)>& fn);
    bool readPagemapEntries(uint64_t startAddr, size_t pageCount, std::vector<uint64_t>& out);
    int ensureMemFd();

    int pid_ = -1;
    bool open_ = false;
    bool vmReadvUnsupported_ = false;
    bool memFdAttempted_ = false;
    int memFd_ = -1;
    int pagemapFd_ = -1;
    uint8_t* scanBuf_ = nullptr;

    std::vector<Region> allRegions_;
    std::vector<Region> heapRegions_;
    std::vector<Region> roFileRegions_;
    LiveBitmapInfo mainBitmap_;

    std::unordered_map<uint64_t, std::array<uint8_t, kPageSize>> pages_;
    std::deque<uint64_t> pageOrder_;
    uint64_t pageReads_ = 0;
};

} // namespace artseek
