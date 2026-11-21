#pragma once

#include "mem.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace artseek {

struct DexFieldName {
    std::string cls;
    std::string type;
    std::string name;
};

class DexFile {
public:
    static std::shared_ptr<DexFile> load(ProcMem& mem, uint64_t begin);

    uint64_t begin() const { return begin_; }
    bool compact() const { return compact_; }
    uint32_t stringIdsSize() const { return stringIdsSize_; }
    uint32_t typeIdsSize() const { return typeIdsSize_; }
    uint32_t fieldIdsSize() const { return fieldIdsSize_; }
    uint32_t classDefsSize() const { return classDefsSize_; }
    uint32_t classDefsOff() const { return classDefsOff_; }

    uint32_t u32(uint32_t off) const { return mem_->u32(begin_ + off); }
    uint16_t u16(uint32_t off) const { return mem_->u16(begin_ + off); }
    uint8_t u8(uint32_t off) const { return mem_->u8(begin_ + off); }

    std::string str(uint32_t idx);
    std::string typeDesc(uint32_t tidx);
    DexFieldName field(uint32_t fidx);

    void rebindMem(ProcMem* m) { mem_ = m; }

private:
    std::pair<uint32_t, uint32_t> uleb(uint32_t off) const;
    static std::string decodeMutf8(const uint8_t* data, size_t maxBytes);

    ProcMem* mem_ = nullptr;
    uint64_t begin_ = 0;
    bool compact_ = false;
    uint32_t stringIdsSize_ = 0;
    uint32_t stringIdsOff_ = 0;
    uint32_t typeIdsSize_ = 0;
    uint32_t typeIdsOff_ = 0;
    uint32_t protoIdsOff_ = 0;
    uint32_t fieldIdsSize_ = 0;
    uint32_t fieldIdsOff_ = 0;
    uint32_t methodIdsSize_ = 0;
    uint32_t methodIdsOff_ = 0;
    uint32_t classDefsSize_ = 0;
    uint32_t classDefsOff_ = 0;
    uint32_t dataSize_ = 0;
    uint32_t dataOff_ = 0;

    std::unordered_map<uint32_t, std::string> strCache_;
    std::unordered_map<uint32_t, std::string> typeCache_;
    std::unordered_map<uint32_t, DexFieldName> fieldCache_;
};

} // namespace artseek
