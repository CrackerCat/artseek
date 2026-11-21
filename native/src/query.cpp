#include "query.hpp"
#include "collections.hpp"
#include "view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <queue>
#include <unordered_set>

namespace artseek {

namespace {

// ---------------- Query VM Value ----------------

struct QState {
    bool isCollection = false;
    bool materialized = false;
    FieldVal single = FieldVal::makeNull();
    std::vector<FieldVal> items;
    Json terminalJson = Json(nullptr);
};

// Navigate a single field or Map key on an ObjRef
FieldVal stepFieldSingle(ArtRuntime& rt, const FieldVal& val, const std::string& name) {
    if (!val.isRef || val.ref < 0x1000) return FieldVal::makeNull();

    // Special virtual properties
    if (name == "$addr") return FieldVal::makePrim(Json(hexAddr(val.ref)));
    if (name == "$class") {
        uint64_t kl = rt.mem().u32(val.ref + rt.layout().klass_off);
        return FieldVal::makePrim(Json(rt.dottedName(kl)));
    }

    // If Map / JSONObject / Bundle, look up key first!
    if (isMapLike(rt, val.ref)) {
        if (auto mv = lookupMapKey(rt, val.ref, name)) {
            return *mv;
        }
    }

    // Regular Java instance field (superclass chain included)
    if (auto fv = readInstanceFieldVal(rt, val.ref, name)) {
        return *fv;
    }
    return FieldVal::makeNull();
}

// Evaluate dotted path like "a.b.c" on a FieldVal (for `where`)
FieldVal evalDottedPath(ArtRuntime& rt, const FieldVal& root, const std::string& path) {
    FieldVal cur = root;
    size_t start = 0;
    while (start <= path.size()) {
        size_t dot = path.find('.', start);
        std::string seg = (dot == std::string::npos) ? path.substr(start) : path.substr(start, dot - start);
        if (!seg.empty()) {
            cur = stepFieldSingle(rt, cur, seg);
            if (!cur.isRef && cur.prim.isNull()) return cur;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return cur;
}

// Convert FieldVal to comparable primitive Json (unwrapping String/CharSequence & Boxed primitives)
Json toComparableJson(ArtRuntime& rt, const FieldVal& fv) {
    if (!fv.isRef) return fv.prim;
    if (fv.ref < 0x1000) return Json(nullptr);
    if (isStringOrCharSeq(rt, fv.ref)) {
        return Json(rt.readCharSeq(fv.ref));
    }
    if (auto boxed = unboxPrimitive(rt, fv.ref)) {
        return *boxed;
    }
    return Json(hexAddr(fv.ref));
}

bool jsonEquals(const Json& a, const Json& b) {
    if (a.isNull() && b.isNull()) return true;
    if (a.isNull() || b.isNull()) return false;
    if (a.isBool() && b.isBool()) return a.asBool() == b.asBool();
    if (a.isNumber() && b.isNumber()) {
        if (a.isInt() && b.isInt()) return a.asInt() == b.asInt();
        return std::fabs(a.asDouble() - b.asDouble()) < 1e-9;
    }
    if (a.isString() && b.isString()) return a.asString() == b.asString();
    // Allow string-vs-number match only if exact? Better keep strict unless numeric op
    return false;
}

std::optional<double> asNumeric(const Json& j) {
    if (j.isNumber()) return j.asDouble();
    if (j.isString() && !j.asString().empty()) {
        char* endp = nullptr;
        double v = std::strtod(j.asString().c_str(), &endp);
        if (endp && *endp == '\0' && std::isfinite(v)) return v;
    }
    return std::nullopt;
}

bool matchesCondition(const Json& actual, const Json& cond) {
    if (cond.isObject()) {
        // Check if object has operator keys ($contains, $startsWith, $endsWith, $gt, $gte, $lt, $lte, $ne, $eq)
        bool hasOp = false;
        for (const auto& [k, _] : cond.asObject()) {
            if (!k.empty() && k[0] == '$') { hasOp = true; break; }
        }
        if (hasOp) {
            for (const auto& [op, rhs] : cond.asObject()) {
                if (op == "$contains") {
                    if (!actual.isString() || !rhs.isString()) return false;
                    if (actual.asString().find(rhs.asString()) == std::string::npos) return false;
                } else if (op == "$startsWith") {
                    if (!actual.isString() || !rhs.isString()) return false;
                    if (actual.asString().rfind(rhs.asString(), 0) != 0) return false;
                } else if (op == "$endsWith") {
                    if (!actual.isString() || !rhs.isString()) return false;
                    const auto& s = actual.asString();
                    const auto& suf = rhs.asString();
                    if (s.size() < suf.size() || s.compare(s.size() - suf.size(), suf.size(), suf) != 0) return false;
                } else if (op == "$gt" || op == "$gte" || op == "$lt" || op == "$lte") {
                    auto lv = asNumeric(actual);
                    auto rv = asNumeric(rhs);
                    if (!lv || !rv) return false;
                    if (op == "$gt" && !(*lv > *rv)) return false;
                    if (op == "$gte" && !(*lv >= *rv)) return false;
                    if (op == "$lt" && !(*lv < *rv)) return false;
                    if (op == "$lte" && !(*lv <= *rv)) return false;
                } else if (op == "$ne") {
                    if (jsonEquals(actual, rhs)) return false;
                } else if (op == "$eq") {
                    if (!jsonEquals(actual, rhs)) return false;
                } else if (op == "$exists") {
                    bool exists = !actual.isNull();
                    if (exists != rhs.asBool(true)) return false;
                }
            }
            return true;
        }
    }
    return jsonEquals(actual, cond);
}

// ---------------- Select Path Parser & Evaluator ----------------

struct SelectSeg {
    std::string field;
    bool hasIndex = false;
    int64_t index = 0;
    bool isWildcard = false;
};

std::vector<SelectSeg> parseSelectPath(const std::string& expr) {
    std::vector<SelectSeg> segs;
    size_t i = 0;
    while (i < expr.size()) {
        if (expr[i] == '.') { ++i; continue; }
        SelectSeg seg;
        size_t start = i;
        while (i < expr.size() && expr[i] != '.' && expr[i] != '[') ++i;
        seg.field = expr.substr(start, i - start);
        if (i < expr.size() && expr[i] == '[') {
            ++i;
            size_t bStart = i;
            while (i < expr.size() && expr[i] != ']') ++i;
            std::string inside = expr.substr(bStart, i - bStart);
            if (i < expr.size() && expr[i] == ']') ++i;
            if (inside == "*") {
                seg.isWildcard = true;
            } else if (!inside.empty()) {
                seg.hasIndex = true;
                seg.index = std::strtoll(inside.c_str(), nullptr, 10);
            }
        }
        if (!seg.field.empty() || seg.hasIndex || seg.isWildcard) {
            segs.push_back(std::move(seg));
        }
    }
    return segs;
}

Json evalSelectSegs(ArtRuntime& rt, const FieldVal& cur,
                    const std::vector<SelectSeg>& segs, size_t idx) {
    if (idx >= segs.size()) {
        return materializeDefault(rt, cur);
    }
    const auto& seg = segs[idx];
    FieldVal next = cur;
    if (!seg.field.empty()) {
        next = stepFieldSingle(rt, cur, seg.field);
    }
    if (seg.isWildcard) {
        Json arr = Json::array();
        if (next.isRef && next.ref >= 0x1000) {
            std::vector<FieldVal> elems;
            if (unpackList(rt, next.ref, elems, 256)) {
                for (const auto& e : elems) {
                    arr.push(evalSelectSegs(rt, e, segs, idx + 1));
                }
            } else if (isMapLike(rt, next.ref)) {
                std::vector<std::pair<std::string, FieldVal>> entries;
                if (unpackMap(rt, next.ref, entries, 256)) {
                    for (const auto& [_, v] : entries) {
                        arr.push(evalSelectSegs(rt, v, segs, idx + 1));
                    }
                }
            }
        }
        return arr;
    }
    if (seg.hasIndex) {
        if (!next.isRef || next.ref < 0x1000) return Json(nullptr);
        std::vector<FieldVal> elems;
        if (!unpackList(rt, next.ref, elems, 4096) || elems.empty()) return Json(nullptr);
        int64_t pos = seg.index;
        if (pos < 0) pos += static_cast<int64_t>(elems.size());
        if (pos < 0 || pos >= static_cast<int64_t>(elems.size())) return Json(nullptr);
        return evalSelectSegs(rt, elems[static_cast<size_t>(pos)], segs, idx + 1);
    }
    return evalSelectSegs(rt, next, segs, idx + 1);
}

Json applySelectSingle(ArtRuntime& rt, const FieldVal& fv, const Json& shape) {
    if (!fv.isRef && fv.prim.isNull()) return Json(nullptr);
    Json out = Json::object();
    for (const auto& [outKey, exprJson] : shape.asObject()) {
        if (!exprJson.isString()) continue;
        auto segs = parseSelectPath(exprJson.asString());
        out.set(outKey, evalSelectSegs(rt, fv, segs, 0));
    }
    return out;
}

// Reverse BFS via batched WhoRefs to find nearest owner matching instanceOf
FieldVal findOwnerSingle(ArtRuntime& rt, const FieldVal& fv,
                         const std::string& instanceOf, int maxHops) {
    if (!fv.isRef || fv.ref < 0x1000) return FieldVal::makeNull();
    uint64_t kl = rt.mem().u32(fv.ref + rt.layout().klass_off);
    if (kl >= 0x1000 && rt.isInstanceOf(kl, instanceOf)) {
        return fv; // already matches instanceOf!
    }

    std::unordered_set<uint64_t> visited;
    visited.insert(fv.ref);
    std::vector<uint32_t> frontier = {static_cast<uint32_t>(fv.ref)};

    for (int hop = 0; hop < maxHops && !frontier.empty(); ++hop) {
        auto batch = rt.whoRefsBatch(frontier, 32);
        std::vector<uint32_t> nextFrontier;
        for (uint32_t tgt : frontier) {
            auto it = batch.find(tgt);
            if (it == batch.end()) continue;
            for (const auto& ro : it->second) {
                if (ro.ownerAddr < 0x1000 || !visited.insert(ro.ownerAddr).second) continue;
                uint64_t okl = rt.mem().u32(ro.ownerAddr + rt.layout().klass_off);
                if (okl >= 0x1000 && rt.isInstanceOf(okl, instanceOf)) {
                    return FieldVal::makeRef(ro.ownerAddr);
                }
                if (visited.size() < 512 && nextFrontier.size() < 128) {
                    nextFrontier.push_back(static_cast<uint32_t>(ro.ownerAddr));
                }
            }
        }
        frontier = std::move(nextFrontier);
    }
    return FieldVal::makeNull();
}

FieldVal stepParentSingle(ArtRuntime& rt, const FieldVal& fv, int hops) {
    if (!fv.isRef || fv.ref < 0x1000) return FieldVal::makeNull();
    uint64_t cur = fv.ref;
    for (int i = 0; i < hops && cur >= 0x1000; ++i) {
        uint64_t kl = rt.mem().u32(cur + rt.layout().klass_off);
        if (kl < 0x1000) return FieldVal::makeNull();
        const auto& fl = rt.fieldLayout(kl);
        cur = rt.readRefField(cur, fl, "mParent");
    }
    return FieldVal::makeRef(cur);
}

Json makeRefJson(ArtRuntime& rt, int pid, const FieldVal& fv) {
    if (!fv.isRef || fv.ref < 0x1000) return Json(nullptr);
    uint64_t kl = rt.mem().u32(fv.ref + rt.layout().klass_off);
    return Json::object({
        {"addr", hexAddr(fv.ref)},
        {"className", rt.dottedName(kl)},
        {"pid", pid}
    });
}

Json makeCrawlJson(ArtRuntime& rt, const FieldVal& fv, const std::string& match,
                   int depth, int maxObjects) {
    Json arr = Json::array();
    if (!fv.isRef || fv.ref < 0x1000) return arr;
    auto hits = rt.crawlStrings(fv.ref, match, depth, maxObjects);
    for (const auto& h : hits) {
        arr.push(Json::object({
            {"path", h.path},
            {"value", h.value},
            {"addr", hexAddr(h.addr)}
        }));
    }
    return arr;
}

} // namespace

OpResult opStrSearch(ArtRuntime& rt, const Json& params) {
    if (!params.has("needle") || !params.get("needle").isString() || params.get("needle").asString().empty()) {
        return OpResult::fail("INVALID_QUERY", "strsearch requires non-empty string 'needle'");
    }
    std::string needle = params.get("needle").asString();
    size_t limit = static_cast<size_t>(std::max<int64_t>(1, params.get("limit").asInt(50)));
    bool includePreview = params.get("includeOwnerPreview").asBool(false);

    auto hits = rt.findStringsContaining(needle, limit);
    std::vector<uint32_t> strAddrs;
    strAddrs.reserve(hits.size());
    for (const auto& h : hits) strAddrs.push_back(static_cast<uint32_t>(h.addr));

    auto batchOwners = rt.whoRefsBatch(strAddrs, 32);

    Json hitsArr = Json::array();
    for (const auto& h : hits) {
        Json ownersArr = Json::array();
        auto oit = batchOwners.find(static_cast<uint32_t>(h.addr));
        if (oit != batchOwners.end()) {
            for (const auto& ow : oit->second) {
                Json oj = Json::object();
                oj.set("refAddr", hexAddr(ow.refAddr));
                oj.set("ownerAddr", hexAddr(ow.ownerAddr));
                oj.set("ownerClass", ow.ownerClass);
                oj.set("fieldName", ow.fieldName);
                oj.set("fieldType", ow.fieldType);
                if (includePreview) {
                    DumpOptions dopts;
                    dopts.depth = 1;
                    dopts.expandCollections = false;
                    dopts.maxArrayItems = 16;
                    oj.set("preview", dumpObject(rt, ow.ownerAddr, dopts));
                }
                ownersArr.push(std::move(oj));
            }
        }
        hitsArr.push(Json::object({
            {"addr", hexAddr(h.addr)},
            {"text", h.text},
            {"owners", std::move(ownersArr)}
        }));
    }

    return OpResult::success(Json::object({
        {"needle", needle},
        {"hits", std::move(hitsArr)}
    }));
}

OpResult opDumpObj(ArtRuntime& rt, const Json& params) {
    if (!params.has("addr") || !params.get("addr").isString()) {
        return OpResult::fail("INVALID_QUERY", "dumpobj requires string 'addr'");
    }
    auto addrOpt = parseAddr(params.get("addr").asString());
    if (!addrOpt || *addrOpt < 0x1000 || (*addrOpt & 7) != 0) {
        return OpResult::fail("NOT_FOUND", "invalid or null object address");
    }
    uint64_t addr = *addrOpt;
    if (!rt.mem().isMapped(addr)) {
        return OpResult::fail("NOT_FOUND", "address not mapped: " + hexAddr(addr));
    }
    uint64_t kl = rt.mem().u32(addr + rt.layout().klass_off);
    auto actualDesc = rt.descriptor(kl);
    if (!actualDesc) {
        return OpResult::fail("STALE_REF", "object at " + hexAddr(addr) + " has invalid class pointer");
    }
    if (params.has("expectedClass") && params.get("expectedClass").isString() &&
        !params.get("expectedClass").asString().empty()) {
        std::string want = params.get("expectedClass").asString();
        if (!typeMatchesPattern(*actualDesc, want)) {
            return OpResult::fail("STALE_REF",
                                  "expected class " + want + " but found " + descToDot(*actualDesc));
        }
    }

    DumpOptions opts;
    opts.depth = static_cast<int>(params.get("depth").asInt(1));
    opts.schemaOnly = params.get("schemaOnly").asBool(false);
    opts.expandCollections = params.get("expandCollections").asBool(true);
    opts.maxArrayItems = static_cast<int>(params.get("maxArrayItems").asInt(64));

    return OpResult::success(dumpObject(rt, addr, opts));
}

OpResult opWhoRefs(ArtRuntime& rt, const Json& params) {
    if (!params.has("addr") || !params.get("addr").isString()) {
        return OpResult::fail("INVALID_QUERY", "whorefs requires string 'addr'");
    }
    auto addrOpt = parseAddr(params.get("addr").asString());
    if (!addrOpt || *addrOpt < 0x1000) {
        return OpResult::fail("INVALID_QUERY", "invalid 'addr'");
    }
    size_t limit = static_cast<size_t>(std::max<int64_t>(1, params.get("limit").asInt(128)));
    auto owners = rt.whoRefs(static_cast<uint32_t>(*addrOpt), limit);
    Json arr = Json::array();
    for (const auto& ow : owners) {
        arr.push(Json::object({
            {"refAddr", hexAddr(ow.refAddr)},
            {"ownerAddr", hexAddr(ow.ownerAddr)},
            {"ownerClass", ow.ownerClass},
            {"fieldName", ow.fieldName},
            {"fieldType", ow.fieldType}
        }));
    }
    return OpResult::success(Json::object({
        {"targetAddr", hexAddr(*addrOpt)},
        {"owners", std::move(arr)}
    }));
}

OpResult opInstances(ArtRuntime& rt, const Json& params) {
    if (!params.has("type") || !params.get("type").isString() || params.get("type").asString().empty()) {
        return OpResult::fail("INVALID_QUERY", "instances requires non-empty string 'type'");
    }
    std::string type = params.get("type").asString();
    size_t limit = static_cast<size_t>(std::max<int64_t>(1, params.get("limit").asInt(256)));
    bool includeSubclasses = params.get("includeSubclasses").asBool(false);

    auto classes = rt.findClassesMatching(type, includeSubclasses);
    std::unordered_set<uint32_t> targetSet;
    for (uint64_t c : classes) targetSet.insert(static_cast<uint32_t>(c));

    uint64_t total = 0;
    Json items = Json::array();
    if (!targetSet.empty()) {
        auto excluded = rt.collectClassMetadataRanges(classes);
        rt.mem().eachHeapWord8([&](uint64_t a, uint32_t w0) -> bool {
            if (targetSet.count(w0)) {
                if (!rt.isPlausibleInstance(a, static_cast<uint64_t>(w0), excluded)) {
                    return true;
                }
                ++total;
                if (items.size() < limit) {
                    items.push(Json::object({
                        {"addr", hexAddr(a)},
                        {"className", rt.dottedName(w0)}
                    }));
                }
            }
            return true;
        }, true);
    }

    return OpResult::success(Json::object({
        {"type", type},
        {"total", total},
        {"items", std::move(items)}
    }));
}

OpResult opQuery(ArtRuntime& rt, int pid, const Json& params) {
    if (!params.has("root") || !params.get("root").isObject()) {
        return OpResult::fail("INVALID_QUERY", "query requires object 'root'");
    }
    const Json& root = params.get("root");
    std::string kind = root.get("kind").asString();
    bool wantCollection = params.get("collection").asBool(false);

    QState state;

    // ---------- 1) Resolve RootSource ----------
    if (kind == "activity") {
        std::string which = root.get("which").asString("current");
        std::string pattern = root.get("pattern").asString();
        if (which == "current") {
            uint64_t act = findCurrentActivity(rt);
            if (act >= 0x1000 && !pattern.empty()) {
                uint64_t kl = rt.mem().u32(act + rt.layout().klass_off);
                auto d = rt.descriptor(kl);
                if (!d || !typeMatchesPattern(*d, pattern)) act = 0;
            }
            state.isCollection = false;
            state.single = FieldVal::makeRef(act);
        } else if (which == "all") {
            auto acts = findActivities(rt);
            state.isCollection = true;
            for (const auto& a : acts) {
                if (!pattern.empty()) {
                    uint64_t kl = rt.mem().u32(a.addr + rt.layout().klass_off);
                    auto d = rt.descriptor(kl);
                    if (!d || !typeMatchesPattern(*d, pattern)) continue;
                }
                state.items.push_back(FieldVal::makeRef(a.addr));
            }
        } else {
            return OpResult::fail("INVALID_QUERY", "activity.which must be 'current' or 'all'");
        }
    } else if (kind == "scan") {
        std::string type = root.get("type").asString();
        if (type.empty()) {
            return OpResult::fail("INVALID_QUERY", "scan root requires 'type'");
        }
        size_t limit = static_cast<size_t>(std::max<int64_t>(1, root.get("limit").asInt(256)));
        bool incSub = root.get("includeSubclasses").asBool(false);
        auto classes = rt.findClassesMatching(type, incSub);
        auto insts = rt.scanInstances(classes, limit);
        state.isCollection = true;
        state.items.reserve(insts.size());
        for (const auto& [addr, _] : insts) {
            state.items.push_back(FieldVal::makeRef(addr));
        }
    } else if (kind == "view") {
        ViewFilter vf;
        if (root.has("a11yViewId") && root.get("a11yViewId").isNumber()) {
            vf.hasA11yViewId = true;
            vf.a11yViewId = static_cast<int32_t>(root.get("a11yViewId").asInt(-1));
        }
        if (root.has("bounds")) {
            vf.bounds = Rect::fromJson(root.get("bounds"));
        }
        if (root.has("resourceIdInt") && root.get("resourceIdInt").isNumber()) {
            vf.hasResourceIdInt = true;
            vf.resourceIdInt = static_cast<int32_t>(root.get("resourceIdInt").asInt(0));
        }
        if (root.has("text") && root.get("text").isString()) {
            vf.hasText = true;
            vf.text = root.get("text").asString();
        }
        if (root.has("desc") && root.get("desc").isString()) {
            vf.hasDesc = true;
            vf.desc = root.get("desc").asString();
        }
        if (root.has("className") && root.get("className").isString()) {
            vf.hasClassName = true;
            vf.className = root.get("className").asString();
        }
        vf.onlyFocused = root.get("onlyFocused").asBool(false);

        uint64_t v = findViewByFilter(rt, vf);
        state.isCollection = false;
        state.single = FieldVal::makeRef(v);
    } else if (kind == "addr") {
        auto addrOpt = parseAddr(root.get("addr").asString());
        if (!addrOpt || *addrOpt < 0x1000 || (*addrOpt & 7) != 0) {
            return OpResult::fail("STALE_REF", "invalid object address in addr root");
        }
        uint64_t addr = *addrOpt;
        if (!rt.mem().isMapped(addr)) {
            return OpResult::fail("STALE_REF", "address unmapped: " + hexAddr(addr));
        }
        uint64_t kl = rt.mem().u32(addr + rt.layout().klass_off);
        auto actualDesc = rt.descriptor(kl);
        if (!actualDesc) {
            return OpResult::fail("STALE_REF", "object class unreadable at " + hexAddr(addr));
        }
        if (root.has("expectedClass") && root.get("expectedClass").isString() &&
            !root.get("expectedClass").asString().empty()) {
            std::string want = root.get("expectedClass").asString();
            if (!typeMatchesPattern(*actualDesc, want)) {
                return OpResult::fail("STALE_REF",
                                      "expectedClass " + want + " mismatch: actual " + descToDot(*actualDesc));
            }
        }
        state.isCollection = false;
        state.single = FieldVal::makeRef(addr);
    } else {
        return OpResult::fail("INVALID_QUERY", "unknown root.kind: " + kind);
    }

    // ---------- 2) Execute Steps Left-to-Right ----------
    const Json& steps = params.get("steps");
    if (steps.isArray()) {
        for (const Json& step : steps.asArray()) {
            if (!step.isObject()) {
                return OpResult::fail("INVALID_QUERY", "step must be an object");
            }
            if (state.materialized) {
                return OpResult::fail("INVALID_QUERY", "cannot execute step after terminal step");
            }
            std::string sop = step.get("op").asString();

            if (sop == "field") {
                std::string name = step.get("name").asString();
                if (name.empty()) return OpResult::fail("INVALID_QUERY", "field step requires 'name'");
                if (state.isCollection) {
                    for (auto& it : state.items) it = stepFieldSingle(rt, it, name);
                } else {
                    state.single = stepFieldSingle(rt, state.single, name);
                }
            } else if (sop == "index") {
                int64_t idx = step.get("index").asInt(0);
                if (state.isCollection) {
                    int64_t pos = idx;
                    if (pos < 0) pos += static_cast<int64_t>(state.items.size());
                    FieldVal picked = (pos >= 0 && pos < static_cast<int64_t>(state.items.size()))
                                          ? state.items[static_cast<size_t>(pos)]
                                          : FieldVal::makeNull();
                    state.isCollection = false;
                    state.items.clear();
                    state.single = std::move(picked);
                } else {
                    if (!state.single.isRef || state.single.ref < 0x1000) {
                        state.single = FieldVal::makeNull();
                    } else {
                        std::vector<FieldVal> elems;
                        if (unpackList(rt, state.single.ref, elems, 4096) && !elems.empty()) {
                            int64_t pos = idx;
                            if (pos < 0) pos += static_cast<int64_t>(elems.size());
                            if (pos >= 0 && pos < static_cast<int64_t>(elems.size())) {
                                state.single = elems[static_cast<size_t>(pos)];
                            } else {
                                state.single = FieldVal::makeNull();
                            }
                        } else {
                            state.single = FieldVal::makeNull();
                        }
                    }
                }
            } else if (sop == "first") {
                if (state.isCollection) {
                    FieldVal picked = FieldVal::makeNull();
                    for (const auto& it : state.items) {
                        if (it.isRef || !it.prim.isNull()) {
                            picked = it;
                            break;
                        }
                    }
                    state.isCollection = false;
                    state.items.clear();
                    state.single = std::move(picked);
                } else if (state.single.isRef && state.single.ref >= 0x1000 && isListLike(rt, state.single.ref)) {
                    std::vector<FieldVal> elems;
                    FieldVal picked = FieldVal::makeNull();
                    if (unpackList(rt, state.single.ref, elems, 256)) {
                        for (const auto& e : elems) {
                            if (e.isRef || !e.prim.isNull()) {
                                picked = e;
                                break;
                            }
                        }
                    }
                    state.single = std::move(picked);
                }
            } else if (sop == "where") {
                const Json& filter = step.get("filter");
                if (!filter.isObject()) {
                    return OpResult::fail("INVALID_QUERY", "where step requires object 'filter'");
                }
                // If operating on a single Java List/Array, unpack it into a collection first
                if (!state.isCollection) {
                    std::vector<FieldVal> elems;
                    if (state.single.isRef && state.single.ref >= 0x1000 &&
                        unpackList(rt, state.single.ref, elems, 4096)) {
                        state.isCollection = true;
                        state.items = std::move(elems);
                    } else if (state.single.isRef || !state.single.prim.isNull()) {
                        state.isCollection = true;
                        state.items = {state.single};
                    } else {
                        state.isCollection = true;
                        state.items.clear();
                    }
                }
                std::vector<FieldVal> filtered;
                for (const auto& item : state.items) {
                    bool allMatch = true;
                    for (const auto& [path, cond] : filter.asObject()) {
                        FieldVal sub = evalDottedPath(rt, item, path);
                        Json actual = toComparableJson(rt, sub);
                        if (!matchesCondition(actual, cond)) {
                            allMatch = false;
                            break;
                        }
                    }
                    if (allMatch) filtered.push_back(item);
                }
                state.items = std::move(filtered);
            } else if (sop == "parent") {
                int hops = static_cast<int>(std::max<int64_t>(1, step.get("hops").asInt(1)));
                if (state.isCollection) {
                    for (auto& it : state.items) it = stepParentSingle(rt, it, hops);
                } else {
                    state.single = stepParentSingle(rt, state.single, hops);
                }
            } else if (sop == "find_owner") {
                std::string instOf = step.get("instanceOf").asString("android.view.View");
                int maxHops = static_cast<int>(std::max<int64_t>(1, step.get("maxHops").asInt(6)));
                if (state.isCollection) {
                    for (auto& it : state.items) it = findOwnerSingle(rt, it, instOf, maxHops);
                } else {
                    state.single = findOwnerSingle(rt, state.single, instOf, maxHops);
                }
            } else if (sop == "crawl") {
                std::string match = step.get("match").asString();
                int depth = static_cast<int>(step.get("depth").asInt(4));
                int maxObjects = static_cast<int>(step.get("maxObjects").asInt(2000));
                if (state.isCollection) {
                    Json arr = Json::array();
                    for (const auto& it : state.items) {
                        arr.push(makeCrawlJson(rt, it, match, depth, maxObjects));
                    }
                    state.terminalJson = std::move(arr);
                } else {
                    state.terminalJson = makeCrawlJson(rt, state.single, match, depth, maxObjects);
                }
                state.materialized = true;
            } else if (sop == "select") {
                const Json& shape = step.get("shape");
                if (!shape.isObject()) {
                    return OpResult::fail("INVALID_QUERY", "select step requires object 'shape'");
                }
                if (state.isCollection) {
                    Json arr = Json::array();
                    for (const auto& it : state.items) {
                        arr.push(applySelectSingle(rt, it, shape));
                    }
                    state.terminalJson = std::move(arr);
                } else {
                    state.terminalJson = applySelectSingle(rt, state.single, shape);
                }
                state.materialized = true;
            } else if (sop == "dump") {
                DumpOptions dopts;
                dopts.depth = static_cast<int>(step.get("depth").asInt(1));
                dopts.schemaOnly = step.get("schemaOnly").asBool(false);
                dopts.expandCollections = step.get("expandCollections").asBool(true);
                dopts.maxArrayItems = static_cast<int>(step.get("maxArrayItems").asInt(64));
                auto dumpOne = [&](const FieldVal& fv) -> Json {
                    if (!fv.isRef) return fv.prim;
                    return dumpObject(rt, fv.ref, dopts);
                };
                if (state.isCollection) {
                    Json arr = Json::array();
                    for (const auto& it : state.items) arr.push(dumpOne(it));
                    state.terminalJson = std::move(arr);
                } else {
                    state.terminalJson = dumpOne(state.single);
                }
                state.materialized = true;
            } else if (sop == "ref") {
                if (state.isCollection) {
                    Json arr = Json::array();
                    for (const auto& it : state.items) arr.push(makeRefJson(rt, pid, it));
                    state.terminalJson = std::move(arr);
                } else {
                    state.terminalJson = makeRefJson(rt, pid, state.single);
                }
                state.materialized = true;
            } else if (sop == "view_meta") {
                auto vmOne = [&](const FieldVal& fv) -> Json {
                    if (!fv.isRef || fv.ref < 0x1000) return Json(nullptr);
                    return computeViewMeta(rt, fv.ref);
                };
                if (state.isCollection) {
                    Json arr = Json::array();
                    for (const auto& it : state.items) arr.push(vmOne(it));
                    state.terminalJson = std::move(arr);
                } else {
                    state.terminalJson = vmOne(state.single);
                }
                state.materialized = true;
            } else {
                return OpResult::fail("INVALID_QUERY", "unknown step op: " + sop);
            }
        }
    }

    // ---------- 3) Default Terminal Materialization ----------
    Json result;
    if (state.materialized) {
        result = std::move(state.terminalJson);
    } else if (state.isCollection) {
        Json arr = Json::array();
        for (const auto& it : state.items) {
            arr.push(materializeDefault(rt, it));
        }
        result = std::move(arr);
    } else {
        result = materializeDefault(rt, state.single);
    }

    if (wantCollection && !result.isArray()) {
        Json wrapped = Json::array();
        if (!result.isNull()) wrapped.push(std::move(result));
        result = std::move(wrapped);
    }

    return OpResult::success(std::move(result));
}

OpResult dispatchTargetOp(ArtRuntime& rt, int pid, const std::string& op, const Json& params) {
    if (op == "roots") {
        return OpResult::success(opRoots(rt));
    }
    if (op == "tree") {
        bool onlyFocused = params.get("onlyFocused").asBool(false);
        uint64_t filterRoot = 0;
        if (params.has("rootAddr") && params.get("rootAddr").isString()) {
            if (auto a = parseAddr(params.get("rootAddr").asString())) filterRoot = *a;
        }
        int maxDepth = static_cast<int>(params.get("maxDepth").asInt(64));
        return OpResult::success(opTree(rt, onlyFocused, filterRoot, maxDepth));
    }
    if (op == "strsearch") {
        return opStrSearch(rt, params);
    }
    if (op == "dumpobj") {
        return opDumpObj(rt, params);
    }
    if (op == "whorefs") {
        return opWhoRefs(rt, params);
    }
    if (op == "instances") {
        return opInstances(rt, params);
    }
    if (op == "query") {
        return opQuery(rt, pid, params);
    }
    return OpResult::fail("INVALID_QUERY", "unsupported op: " + op);
}

} // namespace artseek
