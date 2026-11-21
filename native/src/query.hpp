#pragma once

#include "art.hpp"
#include "json.hpp"
#include <string>

namespace artseek {

struct OpResult {
    bool ok = true;
    Json data = Json(nullptr);
    std::string errorCode;
    std::string errorMessage;

    static OpResult success(Json d) {
        return OpResult{true, std::move(d), {}, {}};
    }
    static OpResult fail(std::string code, std::string msg) {
        return OpResult{false, Json(nullptr), std::move(code), std::move(msg)};
    }
};

OpResult opStrSearch(ArtRuntime& rt, const Json& params);
OpResult opDumpObj(ArtRuntime& rt, const Json& params);
OpResult opWhoRefs(ArtRuntime& rt, const Json& params);
OpResult opInstances(ArtRuntime& rt, const Json& params);
OpResult opQuery(ArtRuntime& rt, int pid, const Json& params);

// Dispatches any target-bound op (roots, tree, strsearch, dumpobj, whorefs, instances, query)
OpResult dispatchTargetOp(ArtRuntime& rt, int pid, const std::string& op, const Json& params);

} // namespace artseek
