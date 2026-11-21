#pragma once

#include "dex.hpp"
#include "json.hpp"
#include "layout.hpp"
#include "mem.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace artseek {

struct ClassInfo {
    uint64_t addr = 0;
    std::string name;
    uint64_t superClass = 0;
    uint32_t objSize = 0;
    uint32_t classFlags = 0;
    uint32_t status = 0;
    int32_t typeIdx = -1;
    int32_t classDef = -1;
    uint64_t dexCache = 0;
    uint32_t numRefInst = 0;
};

struct ArtFieldInfo {
    uint32_t dexIdx = 0;
    uint32_t offset = 0;
    uint32_t access = 0;
    uint64_t declClass = 0;
    std::string name;
    std::string type;
};

struct FieldLayout {
    std::unordered_map<std::string, ArtFieldInfo> byName;
    std::unordered_map<uint32_t, ArtFieldInfo> byOff;
    std::vector<ArtFieldInfo> fields;    // sorted by offset
    std::vector<ArtFieldInfo> refFields; // only reference fields (L or [)
};

struct RefOwner {
    uint64_t refAddr = 0;
    uint64_t ownerAddr = 0;
    std::string ownerClass; // dotted
    std::string fieldName;
    std::string fieldType;
};

struct CrawlHit {
    std::string path;
    std::string value;
    uint64_t addr = 0;
};

struct StringHit {
    uint64_t addr = 0;
    std::string text;
};

// Descriptor & pattern utilities
std::string descToDot(const std::string& desc);
std::string dotToDesc(const std::string& dot);
std::string shortClassName(const std::string& nameOrDesc);
bool globMatch(std::string_view pattern, std::string_view text);
bool typeMatchesPattern(const std::string& desc, const std::string& pattern);

// UTF-16LE encoding helper
std::vector<uint8_t> encodeUtf16LE(std::string_view utf8);

class ArtRuntime {
public:
    ArtRuntime(ProcMem& mem, const ArtLayout& layout);

    ProcMem& mem() { return mem_; }
    const ArtLayout& layout() const { return L_; }
    void setLayout(const ArtLayout& l);

    // Clear all cached metadata (e.g. on recalibration or process change)
    void clearCache();

    // Begin a new request on the same process (refreshes maps, clears mem page cache, keeps class/dex cache warm)
    bool beginRequest(std::string* err = nullptr);
    void endRequest(bool enableMadvise);

    bool isCacheWarm() const { return classIndexBuilt_ && anchor_ != 0; }

    // String & Class inspection
    std::optional<std::string> decodeString(uint64_t addr, bool strictPrintable = false);
    bool isClassShaped(uint64_t addr);
    std::optional<ClassInfo> readClass(uint64_t addr);

    // Anchor & Dex resolution
    uint64_t anchor();
    std::shared_ptr<DexFile> dexFor(uint64_t dexCache);
    std::string dexLocation(uint64_t dexCache);
    uint64_t findDexBegin(uint64_t dexFilePtr);

    // Class descriptors & hierarchy
    std::optional<std::string> descriptor(uint64_t classAddr);
    std::string dottedName(uint64_t classAddr);
    std::vector<std::string> superClasses(uint64_t classAddr);
    bool isInstanceOf(uint64_t classAddr, const std::string& targetType);
    bool isViewClass(uint64_t classAddr);
    bool isActivityClass(uint64_t classAddr);

    // Full class index (warm-cached)
    void ensureClassIndex();
    uint64_t findClass(const std::string& nameOrDesc);
    std::vector<uint64_t> findClassesMatching(const std::string& pattern, bool includeSubclasses = false);

    uint64_t stringClass();
    uint64_t viewRootImplClass();
    uint64_t activityThreadClass();

    // Instance & static fields
    std::vector<ArtFieldInfo> readRawInstanceFields(uint64_t classAddr);
    const FieldLayout& fieldLayout(uint64_t classAddr);
    const FieldLayout& staticFieldLayout(uint64_t classAddr);

    // Convenience typed field accessors
    uint64_t readRefField(uint64_t obj, const FieldLayout& fl, const std::string& name);
    int32_t readIntField(uint64_t obj, const FieldLayout& fl, const std::string& name, int32_t def = 0);
    int64_t readLongField(uint64_t obj, const FieldLayout& fl, const std::string& name, int64_t def = 0);
    bool readBoolField(uint64_t obj, const FieldLayout& fl, const std::string& name, bool def = false);
    float readFloatField(uint64_t obj, const FieldLayout& fl, const std::string& name, float def = 0.0f);
    double readDoubleField(uint64_t obj, const FieldLayout& fl, const std::string& name, double def = 0.0);

    // CharSequence -> UTF-8 string
    std::string readCharSeq(uint64_t addr);

    // Heap queries
    std::vector<std::pair<uint64_t, uint64_t>> collectClassMetadataRanges(
        const std::vector<uint64_t>& classAddrs);
    bool isPlausibleInstance(uint64_t addr, uint64_t klassAddr,
                             const std::vector<std::pair<uint64_t, uint64_t>>& excludedRanges);
    std::vector<std::pair<uint64_t, uint64_t>> scanInstances(
        const std::vector<uint64_t>& classAddrs, size_t limit);
    std::vector<StringHit> findStringsContaining(const std::string& needle, size_t limit);
    std::vector<RefOwner> whoRefs(uint32_t target, size_t limit = 256);
    std::unordered_map<uint32_t, std::vector<RefOwner>> whoRefsBatch(
        const std::vector<uint32_t>& targets, size_t limitPerTarget = 64);
    std::vector<CrawlHit> crawlStrings(uint64_t root, const std::string& match,
                                       int depth = 4, int maxObjects = 2000);

    // Fast structural check for auto-recalibration trigger
    bool quickValidate(std::string* reason = nullptr);

private:
    static std::string primDescriptor(const std::string& name);
    std::optional<RefOwner> resolveOwner(uint64_t refAddr, uint32_t target);

    ProcMem& mem_;
    ArtLayout L_;

    uint64_t anchor_ = 0;
    uint64_t strClass_ = 0;
    uint64_t viewRootImplClass_ = 0;
    uint64_t activityThreadClass_ = 0;
    bool classIndexBuilt_ = false;

    std::unordered_map<uint64_t, std::shared_ptr<DexFile>> dexByDC_;
    std::unordered_map<uint64_t, std::string> dexLoc_;
    std::unordered_map<uint64_t, std::string> descCache_;
    std::unordered_map<std::string, std::vector<uint64_t>> classesByDesc_;
    std::vector<uint64_t> allClasses_;
    std::unordered_map<uint64_t, FieldLayout> fieldLayouts_;
    std::unordered_map<uint64_t, FieldLayout> staticFieldLayouts_;
    std::unordered_map<uint64_t, bool> isViewCache_;
    std::unordered_map<uint64_t, bool> isActivityCache_;
};

// Warm session cache manager keyed by (pid, starttime)
class RuntimeManager {
public:
    struct Handle {
        ArtRuntime* rt = nullptr;
        uint64_t starttime = 0;
        bool cacheHit = false;
    };

    explicit RuntimeManager(const ArtLayout& layout) : layout_(layout) {}

    const ArtLayout& layout() const { return layout_; }
    void setLayout(const ArtLayout& l);
    void clearAll();

    std::optional<Handle> acquire(int pid, std::string* err = nullptr);

private:
    struct Entry {
        int pid = 0;
        uint64_t starttime = 0;
        std::unique_ptr<ProcMem> mem;
        std::unique_ptr<ArtRuntime> rt;
    };

    ArtLayout layout_;
    std::unique_ptr<Entry> active_;
};

} // namespace artseek
