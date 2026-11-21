#pragma once

#include "art.hpp"
#include "json.hpp"
#include "layout.hpp"
#include <string>
#include <vector>

namespace artseek {

struct CheckItem {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct SelfCheckReport {
    bool passed = false;
    std::vector<CheckItem> checks;
    Json toJson(bool recalibrated, const ArtLayout& layout) const;
};

bool hasEmbeddedSentinel();
size_t embeddedSentinelSize();

bool calibrateWithEmbeddedSentinel(const std::string& cacheDir,
                                   ArtLayout& outLayout,
                                   std::string* err = nullptr);

SelfCheckReport runSelfCheck(ArtRuntime& rt, int pid);

} // namespace artseek
