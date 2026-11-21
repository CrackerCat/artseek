#include "collections.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace artseek {

namespace {

void appendUtf8Char(std::string& out, uint16_t u) {
    if (u <= 0x7F) {
        out.push_back(static_cast<char>(u));
    } else if (u <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (u >> 6)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (u >> 12)));
        out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }
}

bool isDumpSkipField(const std::string& name) {
    static const std::unordered_set<std::string> kSkip = {
        "mParent", "mContext", "mAttachInfo", "mResources",
        "mWindow", "mRootView", "mDisplay", "mDrawingCache",
        "mRenderNode", "mOverlay", "mOutlineProvider",
        "mThread", "mHandler", "mChoreographer",
        "mApplication", "mBase", "mPackageInfo"
    };
    return kSkip.count(name) > 0;
}

std::string stringifyKey(ArtRuntime& rt, uint64_t keyAddr) {
    if (keyAddr < 0x1000) return "null";
    std::string cs = rt.readCharSeq(keyAddr);
    if (!cs.empty()) return cs;
    // Empty String check
    uint64_t kl = rt.mem().u32(keyAddr + rt.layout().klass_off);
    auto d = rt.descriptor(kl);
    if (d && *d == "Ljava/lang/String;") {
        if (auto s = rt.decodeString(keyAddr, false)) return *s;
    }
    if (auto boxed = unboxPrimitive(rt, keyAddr)) {
        if (boxed->isString()) return boxed->asString();
        if (boxed->isBool()) return boxed->asBool() ? "true" : "false";
        if (boxed->isInt()) return std::to_string(boxed->asInt());
        if (boxed->isDouble()) return boxed->dump();
    }
    return (d ? shortClassName(*d) : std::string("Object")) + "@" + hexAddr(keyAddr);
}

uint64_t getSparseDeletedObj(ArtRuntime& rt, uint64_t sparseClass) {
    const auto& sfl = rt.staticFieldLayout(sparseClass);
    auto it = sfl.byName.find("DELETED");
    if (it != sfl.byName.end()) {
        return rt.mem().u32(sparseClass + it->second.offset);
    }
    return 0;
}

} // namespace

bool isStringOrCharSeq(ArtRuntime& rt, uint64_t objAddr) {
    if (objAddr < 0x1000 || (objAddr & 7) != 0) return false;
    uint64_t kl = rt.mem().u32(objAddr + rt.layout().klass_off);
    if (kl < 0x1000) return false;
    if (rt.stringClass() != 0 && kl == rt.stringClass()) return true;
    auto d = rt.descriptor(kl);
    if (!d) return false;
    return *d == "Ljava/lang/String;" ||
           *d == "Ljava/lang/StringBuilder;" ||
           *d == "Ljava/lang/StringBuffer;" ||
           *d == "Landroid/text/SpannableString;" ||
           *d == "Landroid/text/SpannedString;" ||
           *d == "Landroid/text/SpannableStringBuilder;";
}

std::optional<Json> readPrimitiveField(ArtRuntime& rt, uint64_t objAddr, const ArtFieldInfo& fi) {
    if (fi.type.empty()) return std::nullopt;
    uint64_t a = objAddr + fi.offset;
    switch (fi.type[0]) {
        case 'Z': return Json(rt.mem().u8(a) != 0);
        case 'B': return Json(static_cast<int64_t>(static_cast<int8_t>(rt.mem().u8(a))));
        case 'S': return Json(static_cast<int64_t>(static_cast<int16_t>(rt.mem().u16(a))));
        case 'C': {
            std::string s;
            appendUtf8Char(s, rt.mem().u16(a));
            return Json(std::move(s));
        }
        case 'I': return Json(static_cast<int64_t>(rt.mem().i32(a)));
        case 'J': return Json(rt.mem().i64(a));
        case 'F': {
            float f = rt.mem().f32(a);
            return std::isfinite(f) ? Json(static_cast<double>(f)) : Json(nullptr);
        }
        case 'D': {
            double d = rt.mem().f64(a);
            return std::isfinite(d) ? Json(d) : Json(nullptr);
        }
        default:
            return std::nullopt;
    }
}

std::optional<Json> unboxPrimitive(ArtRuntime& rt, uint64_t objAddr) {
    if (objAddr < 0x1000 || (objAddr & 7) != 0) return std::nullopt;
    uint64_t kl = rt.mem().u32(objAddr + rt.layout().klass_off);
    auto d = rt.descriptor(kl);
    if (!d) return std::nullopt;
    if (*d != "Ljava/lang/Integer;" && *d != "Ljava/lang/Long;" &&
        *d != "Ljava/lang/Boolean;" && *d != "Ljava/lang/Double;" &&
        *d != "Ljava/lang/Float;" && *d != "Ljava/lang/Short;" &&
        *d != "Ljava/lang/Byte;" && *d != "Ljava/lang/Character;") {
        return std::nullopt;
    }
    const auto& fl = rt.fieldLayout(kl);
    auto it = fl.byName.find("value");
    if (it == fl.byName.end()) return std::nullopt;
    return readPrimitiveField(rt, objAddr, it->second);
}

std::optional<FieldVal> readInstanceFieldVal(ArtRuntime& rt, uint64_t objAddr, const std::string& fieldName) {
    if (objAddr < 0x1000 || (objAddr & 7) != 0) return std::nullopt;
    uint64_t kl = rt.mem().u32(objAddr + rt.layout().klass_off);
    if (kl < 0x1000) return std::nullopt;
    const auto& fl = rt.fieldLayout(kl);
    auto it = fl.byName.find(fieldName);
    if (it == fl.byName.end()) return std::nullopt;
    const auto& fi = it->second;
    if (!fi.type.empty() && (fi.type[0] == 'L' || fi.type[0] == '[')) {
        uint64_t r = rt.mem().u32(objAddr + fi.offset);
        return FieldVal::makeRef(r);
    }
    if (auto p = readPrimitiveField(rt, objAddr, fi)) {
        return FieldVal::makePrim(std::move(*p));
    }
    return FieldVal::makeNull();
}

bool isListLike(ArtRuntime& rt, uint64_t objAddr) {
    if (objAddr < 0x1000 || (objAddr & 7) != 0) return false;
    uint64_t kl = rt.mem().u32(objAddr + rt.layout().klass_off);
    auto d = rt.descriptor(kl);
    if (!d) return false;
    if ((*d)[0] == '[') return true;
    if (*d == "Ljava/util/ArrayList;" ||
        *d == "Ljava/util/Arrays$ArrayList;" ||
        *d == "Ljava/util/LinkedList;" ||
        *d == "Ljava/util/Vector;" ||
        *d == "Ljava/util/Collections$SingletonList;" ||
        *d == "Ljava/util/Collections$EmptyList;" ||
        *d == "Ljava/util/Collections$UnmodifiableList;" ||
        *d == "Ljava/util/Collections$UnmodifiableRandomAccessList;" ||
        *d == "Ljava/util/Collections$SynchronizedList;" ||
        *d == "Ljava/util/Collections$SynchronizedRandomAccessList;" ||
        *d == "Ljava/util/HashSet;" ||
        *d == "Ljava/util/LinkedHashSet;" ||
        *d == "Ljava/util/Collections$EmptySet;" ||
        *d == "Lorg/json/JSONArray;" ||
        *d == "Landroid/util/SparseArray;" ||
        *d == "Landroid/util/LongSparseArray;" ||
        *d == "Landroidx/collection/SparseArrayCompat;") {
        return true;
    }
    if (rt.isInstanceOf(kl, "Ljava/util/ArrayList;") ||
        rt.isInstanceOf(kl, "Landroid/util/SparseArray;")) {
        return true;
    }
    return false;
}

bool isMapLike(ArtRuntime& rt, uint64_t objAddr) {
    if (objAddr < 0x1000 || (objAddr & 7) != 0) return false;
    uint64_t kl = rt.mem().u32(objAddr + rt.layout().klass_off);
    auto d = rt.descriptor(kl);
    if (!d) return false;
    if (*d == "Ljava/util/HashMap;" ||
        *d == "Ljava/util/LinkedHashMap;" ||
        *d == "Ljava/util/concurrent/ConcurrentHashMap;" ||
        *d == "Landroid/util/ArrayMap;" ||
        *d == "Landroidx/collection/ArrayMap;" ||
        *d == "Landroidx/collection/SimpleArrayMap;" ||
        *d == "Lorg/json/JSONObject;" ||
        *d == "Landroid/os/Bundle;" ||
        *d == "Landroid/os/BaseBundle;" ||
        *d == "Ljava/util/Collections$EmptyMap;" ||
        *d == "Ljava/util/Collections$UnmodifiableMap;" ||
        *d == "Ljava/util/Collections$SynchronizedMap;" ||
        *d == "Ljava/util/Collections$SingletonMap;") {
        return true;
    }
    if (rt.isInstanceOf(kl, "Ljava/util/HashMap;") ||
        rt.isInstanceOf(kl, "Landroid/util/ArrayMap;") ||
        rt.isInstanceOf(kl, "Landroidx/collection/SimpleArrayMap;") ||
        rt.isInstanceOf(kl, "Landroid/os/BaseBundle;")) {
        return true;
    }
    return false;
}

bool isCollectionLike(ArtRuntime& rt, uint64_t objAddr) {
    return isListLike(rt, objAddr) || isMapLike(rt, objAddr);
}

static bool unpackListImpl(ArtRuntime& rt, uint64_t objAddr, std::vector<FieldVal>& out,
                           size_t maxItems, int depth) {
    if (depth > 6 || objAddr < 0x1000 || (objAddr & 7) != 0) return false;
    uint64_t kl = rt.mem().u32(objAddr + rt.layout().klass_off);
    auto d = rt.descriptor(kl);
    if (!d) return false;

    // 1) Java Array
    if ((*d)[0] == '[' && d->size() >= 2) {
        int32_t len = rt.mem().i32(objAddr + 8);
        if (len < 0 || len > 10000000) return false;
        size_t n = std::min<size_t>(static_cast<size_t>(len), maxItems);
        out.reserve(n);
        char elem = (*d)[1];
        switch (elem) {
            case 'L':
            case '[':
                for (size_t i = 0; i < n; ++i) {
                    out.push_back(FieldVal::makeRef(rt.mem().u32(objAddr + 12 + i * 4)));
                }
                return true;
            case 'Z':
                for (size_t i = 0; i < n; ++i) {
                    out.push_back(FieldVal::makePrim(Json(rt.mem().u8(objAddr + 12 + i) != 0)));
                }
                return true;
            case 'B':
                for (size_t i = 0; i < n; ++i) {
                    out.push_back(FieldVal::makePrim(Json(static_cast<int64_t>(static_cast<int8_t>(rt.mem().u8(objAddr + 12 + i))))));
                }
                return true;
            case 'S':
                for (size_t i = 0; i < n; ++i) {
                    out.push_back(FieldVal::makePrim(Json(static_cast<int64_t>(static_cast<int16_t>(rt.mem().u16(objAddr + 12 + i * 2))))));
                }
                return true;
            case 'C':
                for (size_t i = 0; i < n; ++i) {
                    std::string s;
                    appendUtf8Char(s, rt.mem().u16(objAddr + 12 + i * 2));
                    out.push_back(FieldVal::makePrim(Json(std::move(s))));
                }
                return true;
            case 'I':
                for (size_t i = 0; i < n; ++i) {
                    out.push_back(FieldVal::makePrim(Json(static_cast<int64_t>(rt.mem().i32(objAddr + 12 + i * 4)))));
                }
                return true;
            case 'F':
                for (size_t i = 0; i < n; ++i) {
                    float f = rt.mem().f32(objAddr + 12 + i * 4);
                    out.push_back(FieldVal::makePrim(std::isfinite(f) ? Json(static_cast<double>(f)) : Json(nullptr)));
                }
                return true;
            case 'J':
                // ART LongArray data starts at offset +16 (8-byte aligned)!
                for (size_t i = 0; i < n; ++i) {
                    out.push_back(FieldVal::makePrim(Json(rt.mem().i64(objAddr + 16 + i * 8))));
                }
                return true;
            case 'D':
                // ART DoubleArray data starts at offset +16 (8-byte aligned)!
                for (size_t i = 0; i < n; ++i) {
                    double v = rt.mem().f64(objAddr + 16 + i * 8);
                    out.push_back(FieldVal::makePrim(std::isfinite(v) ? Json(v) : Json(nullptr)));
                }
                return true;
            default:
                return false;
        }
    }

    const auto& fl = rt.fieldLayout(kl);

    // 2) EmptyList / EmptySet
    if (*d == "Ljava/util/Collections$EmptyList;" || *d == "Ljava/util/Collections$EmptySet;") {
        return true;
    }

    // 3) SingletonList
    if (*d == "Ljava/util/Collections$SingletonList;") {
        out.push_back(FieldVal::makeRef(rt.readRefField(objAddr, fl, "element")));
        return true;
    }

    // 4) Unmodifiable / Synchronized wrappers
    if (*d == "Ljava/util/Collections$UnmodifiableList;" ||
        *d == "Ljava/util/Collections$UnmodifiableRandomAccessList;" ||
        *d == "Ljava/util/Collections$SynchronizedList;" ||
        *d == "Ljava/util/Collections$SynchronizedRandomAccessList;") {
        uint64_t inner = rt.readRefField(objAddr, fl, "list");
        if (inner == 0) inner = rt.readRefField(objAddr, fl, "c");
        return unpackListImpl(rt, inner, out, maxItems, depth + 1);
    }

    // 5) Arrays$ArrayList
    if (*d == "Ljava/util/Arrays$ArrayList;") {
        uint64_t arr = rt.readRefField(objAddr, fl, "a");
        if (arr < 0x1000) return true;
        return unpackListImpl(rt, arr, out, maxItems, depth + 1);
    }

    // 6) ArrayList / Vector / subclasses
    if (*d == "Ljava/util/ArrayList;" || *d == "Ljava/util/Vector;" ||
        rt.isInstanceOf(kl, "Ljava/util/ArrayList;")) {
        uint64_t arr = rt.readRefField(objAddr, fl, "elementData");
        int32_t sz = rt.readIntField(objAddr, fl, "size", -1);
        if (sz < 0) sz = rt.readIntField(objAddr, fl, "elementCount", 0);
        if (arr < 0x1000 || sz <= 0) return true;
        int32_t alen = rt.mem().i32(arr + 8);
        int32_t lim = std::min({sz, alen, static_cast<int32_t>(maxItems)});
        if (lim < 0) lim = 0;
        out.reserve(static_cast<size_t>(lim));
        for (int32_t i = 0; i < lim; ++i) {
            out.push_back(FieldVal::makeRef(rt.mem().u32(arr + 12 + static_cast<uint64_t>(i) * 4)));
        }
        return true;
    }

    // 7) JSONArray
    if (*d == "Lorg/json/JSONArray;") {
        uint64_t vals = rt.readRefField(objAddr, fl, "values");
        return unpackListImpl(rt, vals, out, maxItems, depth + 1);
    }

    // 8) LinkedList
    if (*d == "Ljava/util/LinkedList;") {
        uint64_t node = rt.readRefField(objAddr, fl, "first");
        int32_t sz = rt.readIntField(objAddr, fl, "size", static_cast<int32_t>(maxItems));
        size_t lim = std::min<size_t>(sz > 0 ? static_cast<size_t>(sz) : 0, maxItems);
        std::unordered_set<uint64_t> seen;
        while (node >= 0x1000 && out.size() < lim && seen.insert(node).second) {
            uint64_t nkl = rt.mem().u32(node + rt.layout().klass_off);
            const auto& nfl = rt.fieldLayout(nkl);
            out.push_back(FieldVal::makeRef(rt.readRefField(node, nfl, "item")));
            node = rt.readRefField(node, nfl, "next");
        }
        return true;
    }

    // 9) HashSet / LinkedHashSet -> extract map keys
    if (*d == "Ljava/util/HashSet;" || *d == "Ljava/util/LinkedHashSet;") {
        uint64_t mapObj = rt.readRefField(objAddr, fl, "map");
        if (mapObj < 0x1000) return true;
        // Read HashMap keys directly as refs
        uint64_t mkl = rt.mem().u32(mapObj + rt.layout().klass_off);
        const auto& mfl = rt.fieldLayout(mkl);
        uint64_t tbl = rt.readRefField(mapObj, mfl, "table");
        if (tbl < 0x1000) return true;
        int32_t tlen = rt.mem().i32(tbl + 8);
        if (tlen < 0 || tlen > 1000000) return true;
        std::unordered_set<uint64_t> seenNodes;
        for (int32_t i = 0; i < tlen && out.size() < maxItems; ++i) {
            uint64_t node = rt.mem().u32(tbl + 12 + static_cast<uint64_t>(i) * 4);
            while (node >= 0x1000 && out.size() < maxItems && seenNodes.insert(node).second) {
                uint64_t nkl = rt.mem().u32(node + rt.layout().klass_off);
                const auto& nfl = rt.fieldLayout(nkl);
                out.push_back(FieldVal::makeRef(rt.readRefField(node, nfl, "key")));
                node = rt.readRefField(node, nfl, "next");
            }
        }
        return true;
    }

    // 10) SparseArray / LongSparseArray / SparseArrayCompat (values list)
    if (*d == "Landroid/util/SparseArray;" || *d == "Landroid/util/LongSparseArray;" ||
        *d == "Landroidx/collection/SparseArrayCompat;" ||
        rt.isInstanceOf(kl, "Landroid/util/SparseArray;")) {
        uint64_t vals = rt.readRefField(objAddr, fl, "mValues");
        int32_t sz = rt.readIntField(objAddr, fl, "mSize", 0);
        if (vals < 0x1000 || sz <= 0) return true;
        int32_t alen = rt.mem().i32(vals + 8);
        int32_t lim = std::min(sz, alen);
        uint64_t deleted = getSparseDeletedObj(rt, kl);
        for (int32_t i = 0; i < lim && out.size() < maxItems; ++i) {
            uint64_t v = rt.mem().u32(vals + 12 + static_cast<uint64_t>(i) * 4);
            if (deleted != 0 && v == deleted) continue;
            out.push_back(FieldVal::makeRef(v));
        }
        return true;
    }

    return false;
}

bool unpackList(ArtRuntime& rt, uint64_t objAddr, std::vector<FieldVal>& out, size_t maxItems) {
    out.clear();
    return unpackListImpl(rt, objAddr, out, maxItems, 0);
}

static bool unpackMapImpl(ArtRuntime& rt, uint64_t objAddr,
                          std::vector<std::pair<std::string, FieldVal>>& out,
                          size_t maxItems, int depth) {
    if (depth > 6 || objAddr < 0x1000 || (objAddr & 7) != 0) return false;
    uint64_t kl = rt.mem().u32(objAddr + rt.layout().klass_off);
    auto d = rt.descriptor(kl);
    if (!d) return false;

    if (*d == "Ljava/util/Collections$EmptyMap;") return true;

    const auto& fl = rt.fieldLayout(kl);

    // 1) SingletonMap
    if (*d == "Ljava/util/Collections$SingletonMap;") {
        uint64_t k = rt.readRefField(objAddr, fl, "k");
        uint64_t v = rt.readRefField(objAddr, fl, "v");
        out.emplace_back(stringifyKey(rt, k), FieldVal::makeRef(v));
        return true;
    }

    // 2) UnmodifiableMap / SynchronizedMap
    if (*d == "Ljava/util/Collections$UnmodifiableMap;" ||
        *d == "Ljava/util/Collections$SynchronizedMap;") {
        uint64_t inner = rt.readRefField(objAddr, fl, "m");
        return unpackMapImpl(rt, inner, out, maxItems, depth + 1);
    }

    // 3) JSONObject -> nameValuePairs (LinkedHashMap)
    if (*d == "Lorg/json/JSONObject;") {
        uint64_t nvp = rt.readRefField(objAddr, fl, "nameValuePairs");
        if (nvp < 0x1000) return true;
        return unpackMapImpl(rt, nvp, out, maxItems, depth + 1);
    }

    // 4) Bundle / BaseBundle -> mMap (ArrayMap)
    if (*d == "Landroid/os/Bundle;" || *d == "Landroid/os/BaseBundle;" ||
        rt.isInstanceOf(kl, "Landroid/os/BaseBundle;")) {
        uint64_t mMap = rt.readRefField(objAddr, fl, "mMap");
        if (mMap < 0x1000) return true;
        return unpackMapImpl(rt, mMap, out, maxItems, depth + 1);
    }

    // 5) android.util.ArrayMap / androidx.collection.ArrayMap / SimpleArrayMap
    if (*d == "Landroid/util/ArrayMap;" || *d == "Landroidx/collection/ArrayMap;" ||
        *d == "Landroidx/collection/SimpleArrayMap;" ||
        rt.isInstanceOf(kl, "Landroid/util/ArrayMap;") ||
        rt.isInstanceOf(kl, "Landroidx/collection/SimpleArrayMap;")) {
        uint64_t arr = rt.readRefField(objAddr, fl, "mArray");
        int32_t sz = rt.readIntField(objAddr, fl, "mSize", 0);
        if (arr < 0x1000 || sz <= 0) return true;
        int32_t alen = rt.mem().i32(arr + 8);
        int32_t pairs = std::min({sz, alen / 2, static_cast<int32_t>(maxItems)});
        if (pairs < 0) pairs = 0;
        out.reserve(static_cast<size_t>(pairs));
        for (int32_t i = 0; i < pairs; ++i) {
            uint64_t k = rt.mem().u32(arr + 12 + static_cast<uint64_t>(i * 2) * 4);
            uint64_t v = rt.mem().u32(arr + 12 + static_cast<uint64_t>(i * 2 + 1) * 4);
            out.emplace_back(stringifyKey(rt, k), FieldVal::makeRef(v));
        }
        return true;
    }

    // 6) HashMap / LinkedHashMap / ConcurrentHashMap
    if (*d == "Ljava/util/HashMap;" || *d == "Ljava/util/LinkedHashMap;" ||
        *d == "Ljava/util/concurrent/ConcurrentHashMap;" ||
        rt.isInstanceOf(kl, "Ljava/util/HashMap;")) {
        // For LinkedHashMap, walking head -> after preserves insertion order!
        if (*d == "Ljava/util/LinkedHashMap;" || rt.isInstanceOf(kl, "Ljava/util/LinkedHashMap;")) {
            uint64_t head = rt.readRefField(objAddr, fl, "head");
            if (head >= 0x1000) {
                std::unordered_set<uint64_t> seen;
                uint64_t node = head;
                while (node >= 0x1000 && out.size() < maxItems && seen.insert(node).second) {
                    uint64_t nkl = rt.mem().u32(node + rt.layout().klass_off);
                    const auto& nfl = rt.fieldLayout(nkl);
                    uint64_t k = rt.readRefField(node, nfl, "key");
                    uint64_t v = rt.readRefField(node, nfl, "value");
                    out.emplace_back(stringifyKey(rt, k), FieldVal::makeRef(v));
                    node = rt.readRefField(node, nfl, "after");
                }
                return true;
            }
        }
        uint64_t tbl = rt.readRefField(objAddr, fl, "table");
        if (tbl < 0x1000) return true;
        int32_t tlen = rt.mem().i32(tbl + 8);
        if (tlen < 0 || tlen > 1000000) return true;
        std::unordered_set<uint64_t> seenNodes;
        for (int32_t i = 0; i < tlen && out.size() < maxItems; ++i) {
            uint64_t node = rt.mem().u32(tbl + 12 + static_cast<uint64_t>(i) * 4);
            while (node >= 0x1000 && out.size() < maxItems && seenNodes.insert(node).second) {
                uint64_t nkl = rt.mem().u32(node + rt.layout().klass_off);
                const auto& nfl = rt.fieldLayout(nkl);
                uint64_t k = rt.readRefField(node, nfl, "key");
                uint64_t v = rt.readRefField(node, nfl, "value");
                if (v == 0 && !nfl.byName.count("value")) {
                    v = rt.readRefField(node, nfl, "val"); // ConcurrentHashMap$Node
                }
                out.emplace_back(stringifyKey(rt, k), FieldVal::makeRef(v));
                node = rt.readRefField(node, nfl, "next");
            }
        }
        return true;
    }

    // 7) SparseArray / LongSparseArray -> map of stringified integer keys
    if (*d == "Landroid/util/SparseArray;" || *d == "Landroid/util/LongSparseArray;" ||
        *d == "Landroidx/collection/SparseArrayCompat;" ||
        rt.isInstanceOf(kl, "Landroid/util/SparseArray;")) {
        uint64_t keys = rt.readRefField(objAddr, fl, "mKeys");
        uint64_t vals = rt.readRefField(objAddr, fl, "mValues");
        int32_t sz = rt.readIntField(objAddr, fl, "mSize", 0);
        if (keys < 0x1000 || vals < 0x1000 || sz <= 0) return true;
        int32_t klen = rt.mem().i32(keys + 8);
        int32_t vlen = rt.mem().i32(vals + 8);
        int32_t lim = std::min({sz, klen, vlen});
        bool isLong = (*d == "Landroid/util/LongSparseArray;");
        uint64_t deleted = getSparseDeletedObj(rt, kl);
        for (int32_t i = 0; i < lim && out.size() < maxItems; ++i) {
            uint64_t v = rt.mem().u32(vals + 12 + static_cast<uint64_t>(i) * 4);
            if (deleted != 0 && v == deleted) continue;
            std::string kstr;
            if (isLong) {
                kstr = std::to_string(rt.mem().i64(keys + 16 + static_cast<uint64_t>(i) * 8));
            } else {
                kstr = std::to_string(rt.mem().i32(keys + 12 + static_cast<uint64_t>(i) * 4));
            }
            out.emplace_back(std::move(kstr), FieldVal::makeRef(v));
        }
        return true;
    }

    return false;
}

bool unpackMap(ArtRuntime& rt, uint64_t objAddr,
               std::vector<std::pair<std::string, FieldVal>>& out, size_t maxItems) {
    out.clear();
    return unpackMapImpl(rt, objAddr, out, maxItems, 0);
}

std::optional<FieldVal> lookupMapKey(ArtRuntime& rt, uint64_t objAddr, const std::string& key) {
    std::vector<std::pair<std::string, FieldVal>> entries;
    if (!unpackMap(rt, objAddr, entries, 4096)) return std::nullopt;
    for (auto& [k, v] : entries) {
        if (k == key) return v;
    }
    return std::nullopt;
}

static Json dumpObjRec(ArtRuntime& rt, uint64_t objAddr, int depth,
                       const DumpOptions& opts, std::unordered_set<uint64_t>& active,
                       bool isRoot) {
    if (objAddr < 0x1000 || (objAddr & 7) != 0) return Json(nullptr);
    uint64_t kl = rt.mem().u32(objAddr + rt.layout().klass_off);
    if (kl < 0x1000) {
        return Json::object({{"$addr", hexAddr(objAddr)}, {"$class", "<unknown>"}});
    }
    auto descOpt = rt.descriptor(kl);
    if (!descOpt) {
        return Json::object({{"$addr", hexAddr(objAddr)}, {"$class", "<unresolved>"}});
    }
    const std::string& desc = *descOpt;
    std::string dotClass = descToDot(desc);

    if (opts.schemaOnly) {
        Json out = Json::object();
        out.set("$addr", hexAddr(objAddr));
        out.set("$class", dotClass);
        Json supers = Json::array();
        for (const auto& sc : rt.superClasses(kl)) supers.push(sc);
        out.set("superClasses", std::move(supers));
        Json fields = Json::object();
        const auto& fl = rt.fieldLayout(kl);
        for (const auto& f : fl.fields) {
            if (f.name.empty() || f.name == "shadow$_klass_" || f.name == "shadow$_monitor_") continue;
            fields.set(f.name, descToDot(f.type));
        }
        out.set("fields", std::move(fields));
        return out;
    }

    // String
    if (desc == "Ljava/lang/String;") {
        if (auto s = rt.decodeString(objAddr, false)) {
            return Json(*s);
        }
        return Json(nullptr);
    }

    // Boxed primitives (when expanding collections or nested)
    if (opts.expandCollections && !isRoot) {
        if (auto boxed = unboxPrimitive(rt, objAddr)) {
            return *boxed;
        }
    }

    // CharSequence wrappers (SpannableString, StringBuilder, etc.) when nested
    if (!isRoot && isStringOrCharSeq(rt, objAddr)) {
        return Json(rt.readCharSeq(objAddr));
    }

    // Cycle check
    if (!active.insert(objAddr).second) {
        return Json::object({
            {"$addr", hexAddr(objAddr)},
            {"$class", dotClass},
            {"$cycle", true}
        });
    }
    struct ActiveGuard {
        std::unordered_set<uint64_t>& s;
        uint64_t a;
        ~ActiveGuard() { s.erase(a); }
    } guard{active, objAddr};

    // Collections expansion
    if (opts.expandCollections && depth > 0) {
        if (isMapLike(rt, objAddr)) {
            std::vector<std::pair<std::string, FieldVal>> entries;
            if (unpackMap(rt, objAddr, entries, static_cast<size_t>(std::max(1, opts.maxArrayItems)))) {
                Json m = Json::object();
                for (const auto& [k, fv] : entries) {
                    if (!fv.isRef) {
                        m.set(k, fv.prim);
                    } else {
                        m.set(k, dumpObjRec(rt, fv.ref, depth - 1, opts, active, false));
                    }
                }
                return m;
            }
        }
        if (isListLike(rt, objAddr)) {
            std::vector<FieldVal> elems;
            if (unpackList(rt, objAddr, elems, static_cast<size_t>(std::max(1, opts.maxArrayItems)))) {
                Json a = Json::array();
                for (const auto& fv : elems) {
                    if (!fv.isRef) {
                        a.push(fv.prim);
                    } else {
                        a.push(dumpObjRec(rt, fv.ref, depth - 1, opts, active, false));
                    }
                }
                return a;
            }
        }
    }

    // Stub when depth exhausted
    if (depth <= 0) {
        if (auto boxed = unboxPrimitive(rt, objAddr)) {
            return *boxed;
        }
        return Json::object({{"$addr", hexAddr(objAddr)}, {"$class", dotClass}});
    }

    // Regular Java object dump
    Json out = Json::object();
    out.set("$addr", hexAddr(objAddr));
    out.set("$class", dotClass);

    const auto& fl = rt.fieldLayout(kl);
    for (const auto& f : fl.fields) {
        if (f.name.empty() || f.name == "shadow$_klass_" || f.name == "shadow$_monitor_") {
            continue;
        }
        if (f.type.empty()) continue;
        if (f.type[0] == 'L' || f.type[0] == '[') {
            uint64_t r = rt.mem().u32(objAddr + f.offset);
            if (r < 0x1000) {
                out.set(f.name, Json(nullptr));
                continue;
            }
            // Fast inline String
            uint64_t rkl = rt.mem().u32(r + rt.layout().klass_off);
            if (rt.stringClass() != 0 && rkl == rt.stringClass()) {
                if (auto s = rt.decodeString(r, false)) {
                    out.set(f.name, Json(*s));
                    continue;
                }
            }
            if (isDumpSkipField(f.name)) {
                out.set(f.name, Json::object({
                    {"$addr", hexAddr(r)},
                    {"$class", rt.dottedName(rkl)}
                }));
                continue;
            }
            // Expand nested collections one extra hop if expandCollections is true
            if (opts.expandCollections && isCollectionLike(rt, r)) {
                int nextDepth = (depth > 1) ? (depth - 1) : 1;
                out.set(f.name, dumpObjRec(rt, r, nextDepth, opts, active, false));
                continue;
            }
            out.set(f.name, dumpObjRec(rt, r, depth - 1, opts, active, false));
        } else {
            if (auto pv = readPrimitiveField(rt, objAddr, f)) {
                out.set(f.name, std::move(*pv));
            }
        }
    }
    return out;
}

Json dumpObject(ArtRuntime& rt, uint64_t objAddr, const DumpOptions& opts) {
    std::unordered_set<uint64_t> active;
    return dumpObjRec(rt, objAddr, opts.depth, opts, active, true);
}

Json materializeDefault(ArtRuntime& rt, const FieldVal& val) {
    if (!val.isRef) return val.prim;
    if (val.ref < 0x1000) return Json(nullptr);
    if (isStringOrCharSeq(rt, val.ref)) {
        return Json(rt.readCharSeq(val.ref));
    }
    if (auto boxed = unboxPrimitive(rt, val.ref)) {
        return *boxed;
    }
    if (isCollectionLike(rt, val.ref)) {
        DumpOptions opts;
        opts.depth = 2;
        opts.expandCollections = true;
        opts.maxArrayItems = 64;
        return dumpObject(rt, val.ref, opts);
    }
    DumpOptions opts;
    opts.depth = 1;
    opts.expandCollections = true;
    opts.maxArrayItems = 64;
    return dumpObject(rt, val.ref, opts);
}

} // namespace artseek
