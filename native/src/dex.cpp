#include "dex.hpp"

#include <algorithm>
#include <cstring>

namespace artseek {

static uint32_t readLE32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::shared_ptr<DexFile> DexFile::load(ProcMem& mem, uint64_t begin) {
    if (begin < 0x1000) return nullptr;
    uint8_t h[112];
    if (!mem.readBytes(begin, h, sizeof(h))) return nullptr;
    bool isDex = (std::memcmp(h, "dex\n", 4) == 0);
    bool isCdex = (std::memcmp(h, "cdex", 4) == 0);
    if (!isDex && !isCdex) return nullptr;

    auto d = std::make_shared<DexFile>();
    d->mem_ = &mem;
    d->begin_ = begin;
    d->compact_ = isCdex;
    d->stringIdsSize_ = readLE32(h + 56);
    d->stringIdsOff_ = readLE32(h + 60);
    d->typeIdsSize_ = readLE32(h + 64);
    d->typeIdsOff_ = readLE32(h + 68);
    d->protoIdsOff_ = readLE32(h + 76);
    d->fieldIdsSize_ = readLE32(h + 80);
    d->fieldIdsOff_ = readLE32(h + 84);
    d->methodIdsSize_ = readLE32(h + 88);
    d->methodIdsOff_ = readLE32(h + 92);
    d->classDefsSize_ = readLE32(h + 96);
    d->classDefsOff_ = readLE32(h + 100);
    d->dataSize_ = readLE32(h + 104);
    d->dataOff_ = readLE32(h + 108);

    if (d->stringIdsOff_ == 0 || d->typeIdsOff_ == 0 || d->stringIdsSize_ > (1u << 24)) {
        return nullptr;
    }
    return d;
}

std::pair<uint32_t, uint32_t> DexFile::uleb(uint32_t off) const {
    uint32_t v = 0;
    unsigned shift = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        uint8_t b = u8(off + i);
        v |= static_cast<uint32_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            return {v, off + i + 1};
        }
        shift += 7;
    }
    return {v, off + 5};
}

static void appendUtf8Cp(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string DexFile::decodeMutf8(const uint8_t* data, size_t maxBytes) {
    // MUTF-8 ends at 0x00 byte; decodes UTF-16 surrogate pairs in MUTF-8 into standard UTF-8
    std::string out;
    out.reserve(std::min<size_t>(maxBytes, 128));
    size_t i = 0;
    while (i < maxBytes) {
        uint8_t c = data[i];
        if (c == 0) break;
        if ((c & 0x80) == 0) {
            out.push_back(static_cast<char>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < maxBytes) {
            uint16_t u = static_cast<uint16_t>(((c & 0x1F) << 6) | (data[i + 1] & 0x3F));
            i += 2;
            appendUtf8Cp(out, u);
        } else if ((c & 0xF0) == 0xE0 && i + 2 < maxBytes) {
            uint16_t u = static_cast<uint16_t>(((c & 0x0F) << 12) |
                                               ((data[i + 1] & 0x3F) << 6) |
                                               (data[i + 2] & 0x3F));
            i += 3;
            // Handle surrogate pairs encoded as two 3-byte MUTF-8 sequences
            if (u >= 0xD800 && u <= 0xDBFF && i + 2 < maxBytes && (data[i] & 0xF0) == 0xE0) {
                uint16_t low = static_cast<uint16_t>(((data[i] & 0x0F) << 12) |
                                                     ((data[i + 1] & 0x3F) << 6) |
                                                     (data[i + 2] & 0x3F));
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    i += 3;
                    uint32_t cp = 0x10000 + (((u - 0xD800) << 10) | (low - 0xDC00));
                    appendUtf8Cp(out, cp);
                    continue;
                }
            }
            appendUtf8Cp(out, u);
        } else {
            ++i;
        }
    }
    return out;
}

std::string DexFile::str(uint32_t idx) {
    if (idx >= stringIdsSize_) return {};
    uint32_t off = u32(stringIdsOff_ + idx * 4);
    auto it = strCache_.find(off);
    if (it != strCache_.end()) return it->second;

    auto [utf16Len, dataPos] = uleb(off);
    if (utf16Len > 16384) {
        strCache_[off] = {};
        return {};
    }
    // Check if pure ASCII optimization works or read MUTF-8 up to 3*utf16Len + 1
    size_t readLen = std::min<size_t>(static_cast<size_t>(utf16Len) * 3 + 1, 32768);
    std::vector<uint8_t> raw(readLen);
    mem_->readBytes(begin_ + dataPos, raw.data(), readLen);
    std::string s = decodeMutf8(raw.data(), readLen);
    strCache_[off] = s;
    return s;
}

std::string DexFile::typeDesc(uint32_t tidx) {
    if (tidx >= typeIdsSize_) return {};
    auto it = typeCache_.find(tidx);
    if (it != typeCache_.end()) return it->second;
    uint32_t si = u32(typeIdsOff_ + tidx * 4);
    std::string s = str(si);
    typeCache_[tidx] = s;
    return s;
}

DexFieldName DexFile::field(uint32_t fidx) {
    if (fidx >= fieldIdsSize_) return {};
    auto it = fieldCache_.find(fidx);
    if (it != fieldCache_.end()) return it->second;
    uint32_t base = fieldIdsOff_ + fidx * 8;
    uint32_t classIdx = u16(base);
    uint32_t typeIdx = u16(base + 2);
    uint32_t nameIdx = u32(base + 4);
    DexFieldName f;
    f.cls = typeDesc(classIdx);
    f.type = typeDesc(typeIdx);
    f.name = str(nameIdx);
    fieldCache_[fidx] = f;
    return f;
}

} // namespace artseek
