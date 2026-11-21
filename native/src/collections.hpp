#pragma once

#include "art.hpp"
#include "json.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace artseek {

struct FieldVal {
    bool isRef = false;
    uint64_t ref = 0;
    Json prim; // valid when !isRef (including null)

    static FieldVal makeNull() { return FieldVal{false, 0, Json(nullptr)}; }
    static FieldVal makeRef(uint64_t a) {
        if (a < 0x1000) return makeNull();
        return FieldVal{true, a, Json(nullptr)};
    }
    static FieldVal makePrim(Json j) { return FieldVal{false, 0, std::move(j)}; }
};

struct DumpOptions {
    int depth = 1;
    bool schemaOnly = false;
    bool expandCollections = true;
    int maxArrayItems = 64;
};

bool isStringOrCharSeq(ArtRuntime& rt, uint64_t objAddr);
bool isListLike(ArtRuntime& rt, uint64_t objAddr);
bool isMapLike(ArtRuntime& rt, uint64_t objAddr);
bool isCollectionLike(ArtRuntime& rt, uint64_t objAddr);

std::optional<Json> readPrimitiveField(ArtRuntime& rt, uint64_t objAddr, const ArtFieldInfo& fi);
std::optional<Json> unboxPrimitive(ArtRuntime& rt, uint64_t objAddr);
std::optional<FieldVal> readInstanceFieldVal(ArtRuntime& rt, uint64_t objAddr, const std::string& fieldName);

bool unpackList(ArtRuntime& rt, uint64_t objAddr, std::vector<FieldVal>& out, size_t maxItems = 1024);
bool unpackMap(ArtRuntime& rt, uint64_t objAddr, std::vector<std::pair<std::string, FieldVal>>& out, size_t maxItems = 1024);
std::optional<FieldVal> lookupMapKey(ArtRuntime& rt, uint64_t objAddr, const std::string& key);

Json dumpObject(ArtRuntime& rt, uint64_t objAddr, const DumpOptions& opts);
Json materializeDefault(ArtRuntime& rt, const FieldVal& val);

} // namespace artseek
