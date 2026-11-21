#pragma once

#include "art.hpp"
#include "json.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace artseek {

struct Rect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    int32_t width() const { return right > left ? (right - left) : 0; }
    int32_t height() const { return bottom > top ? (bottom - top) : 0; }
    int64_t area() const { return static_cast<int64_t>(width()) * static_cast<int64_t>(height()); }
    bool empty() const { return right <= left || bottom <= top; }

    Rect intersect(const Rect& o) const {
        int32_t l = std::max(left, o.left);
        int32_t t = std::max(top, o.top);
        int32_t r = std::min(right, o.right);
        int32_t b = std::min(bottom, o.bottom);
        if (r <= l || b <= t) return Rect{0, 0, 0, 0};
        return Rect{l, t, r, b};
    }

    Json toJson() const {
        return Json::object({
            {"left", left},
            {"top", top},
            {"right", right},
            {"bottom", bottom}
        });
    }

    static std::optional<Rect> fromJson(const Json& j) {
        if (!j.isObject()) return std::nullopt;
        if (!j.has("left") || !j.has("top") || !j.has("right") || !j.has("bottom")) {
            return std::nullopt;
        }
        return Rect{
            static_cast<int32_t>(j.get("left").asInt()),
            static_cast<int32_t>(j.get("top").asInt()),
            static_cast<int32_t>(j.get("right").asInt()),
            static_cast<int32_t>(j.get("bottom").asInt())
        };
    }
};

struct ActivityRecordInfo {
    uint64_t addr = 0;
    std::string className;
    bool paused = false;
    bool resumed = false;
    bool stopped = false;
    bool destroyed = false;

    Json toJson() const;
};

struct ViewRootInfo {
    uint64_t addr = 0;
    uint64_t viewAddr = 0;
    std::string viewClass;
    uint64_t activityAddr = 0;
    std::string activityClass;
    int32_t width = 0;
    int32_t height = 0;
    int32_t nodeCount = 0;
    bool added = false;
    bool hasWindowFocus = false;
    int32_t windowVisibility = 0;
    Rect winFrame;
    Rect dispRect;

    Json toJson() const;
};

struct ViewFilter {
    int32_t a11yViewId = -1;
    bool hasA11yViewId = false;
    std::optional<Rect> bounds;
    int32_t resourceIdInt = 0;
    bool hasResourceIdInt = false;
    std::string text;
    bool hasText = false;
    std::string desc;
    bool hasDesc = false;
    std::string className;
    bool hasClassName = false;
    bool onlyFocused = false;
};

// Discovery helpers
uint64_t unwrapActivityContext(ArtRuntime& rt, uint64_t ctxAddr);
std::vector<ViewRootInfo> findViewRoots(ArtRuntime& rt, bool countNodes = true);
std::vector<ActivityRecordInfo> findActivities(ArtRuntime& rt, const std::vector<ViewRootInfo>* knownRoots = nullptr);
uint64_t findCurrentActivity(ArtRuntime& rt);

// Operations
Json opRoots(ArtRuntime& rt);
Json opTree(ArtRuntime& rt, bool onlyFocused, uint64_t filterRootAddr, int maxDepth);
uint64_t findViewByFilter(ArtRuntime& rt, const ViewFilter& filter);
Json computeViewMeta(ArtRuntime& rt, uint64_t viewAddr);

} // namespace artseek
