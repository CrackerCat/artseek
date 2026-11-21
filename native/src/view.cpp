#include "view.hpp"
#include "collections.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace artseek {

Json ActivityRecordInfo::toJson() const {
    return Json::object({
        {"addr", hexAddr(addr)},
        {"className", className},
        {"paused", paused},
        {"resumed", resumed},
        {"stopped", stopped},
        {"destroyed", destroyed}
    });
}

Json ViewRootInfo::toJson() const {
    Json j = Json::object();
    j.set("addr", hexAddr(addr));
    j.set("viewAddr", hexAddr(viewAddr));
    j.set("viewClass", viewClass);
    j.set("activityAddr", activityAddr >= 0x1000 ? Json(hexAddr(activityAddr)) : Json(nullptr));
    j.set("activityClass", !activityClass.empty() ? Json(activityClass) : Json(nullptr));
    j.set("width", width);
    j.set("height", height);
    j.set("nodeCount", nodeCount);
    j.set("added", added);
    j.set("hasWindowFocus", hasWindowFocus);
    j.set("windowVisibility", windowVisibility);
    j.set("winFrame", winFrame.toJson());
    return j;
}

namespace {

Rect readRectObj(ArtRuntime& rt, uint64_t rectAddr) {
    if (rectAddr < 0x1000 || (rectAddr & 7) != 0) return Rect{};
    uint64_t kl = rt.mem().u32(rectAddr + rt.layout().klass_off);
    if (kl < 0x1000) return Rect{};
    const auto& fl = rt.fieldLayout(kl);
    Rect r;
    r.left = rt.readIntField(rectAddr, fl, "left", 0);
    r.top = rt.readIntField(rectAddr, fl, "top", 0);
    r.right = rt.readIntField(rectAddr, fl, "right", 0);
    r.bottom = rt.readIntField(rectAddr, fl, "bottom", 0);
    return r;
}

int32_t countViewSubtree(ArtRuntime& rt, uint64_t v, std::unordered_set<uint64_t>& seen, int depth) {
    if (v < 0x1000 || (v & 7) != 0 || depth > 128 || !seen.insert(v).second) return 0;
    if (seen.size() > 50000) return 0;
    uint64_t kl = rt.mem().u32(v + rt.layout().klass_off);
    if (kl < 0x1000) return 0;
    int32_t total = 1;
    const auto& fl = rt.fieldLayout(kl);
    uint64_t arr = rt.readRefField(v, fl, "mChildren");
    if (arr >= 0x1000) {
        int32_t cnt = rt.readIntField(v, fl, "mChildrenCount", 0);
        int32_t alen = rt.mem().i32(arr + 8);
        int32_t lim = std::min({cnt, alen, 4000});
        for (int32_t i = 0; i < lim; ++i) {
            uint64_t c = rt.mem().u32(arr + 12 + static_cast<uint64_t>(i) * 4);
            if (c >= 0x1000) total += countViewSubtree(rt, c, seen, depth + 1);
        }
    }
    return total;
}

struct BasicViewProps {
    uint64_t addr = 0;
    uint64_t klass = 0;
    std::string className;
    std::vector<std::string> superClasses;
    int32_t a11yViewId = -1;
    int32_t resourceIdInt = -1;
    int32_t viewFlags = 0;
    int32_t visBits = 0; // 0 VISIBLE, 4 INVISIBLE, 8 GONE
    bool clickable = false;
    bool enabled = true;
    bool scrollable = false;
    float alpha = 1.0f;
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    int32_t scrollX = 0, scrollY = 0;
    int32_t transX = 0, transY = 0;
    std::string text;
    std::string desc;
    std::string tag;
    uint64_t parent = 0;
    uint64_t childrenArr = 0;
    int32_t childrenCount = 0;
};

bool isScrollableViewClass(ArtRuntime& rt, uint64_t klass, const std::string& className,
                           const std::vector<std::string>& supers) {
    auto checkName = [](const std::string& n) -> bool {
        std::string sn = shortClassName(n);
        return sn == "RecyclerView" ||
               sn == "ScrollView" ||
               sn == "NestedScrollView" ||
               sn == "HorizontalScrollView" ||
               sn == "AbsListView" ||
               sn == "ListView" ||
               sn == "GridView" ||
               sn == "ViewPager" ||
               sn == "ViewPager2" ||
               sn == "WebView";
    };
    if (checkName(className)) return true;
    for (const auto& s : supers) {
        if (checkName(s)) return true;
    }
    (void)rt;
    (void)klass;
    return false;
}

BasicViewProps readBasicViewProps(ArtRuntime& rt, uint64_t v, bool readStrings = true) {
    BasicViewProps p;
    p.addr = v;
    p.klass = rt.mem().u32(v + rt.layout().klass_off);
    if (p.klass < 0x1000) return p;

    p.className = rt.dottedName(p.klass);
    p.superClasses = rt.superClasses(p.klass);
    const auto& fl = rt.fieldLayout(p.klass);

    p.a11yViewId = rt.readIntField(v, fl, "mAccessibilityViewId", -1);
    p.resourceIdInt = rt.readIntField(v, fl, "mID", -1);
    p.viewFlags = rt.readIntField(v, fl, "mViewFlags", 0);
    p.visBits = p.viewFlags & 0x0000000C;
    p.enabled = (p.viewFlags & 0x00000020) == 0;
    p.clickable = (p.viewFlags & 0x00004000) != 0;

    p.left = rt.readIntField(v, fl, "mLeft", 0);
    p.top = rt.readIntField(v, fl, "mTop", 0);
    p.right = rt.readIntField(v, fl, "mRight", 0);
    p.bottom = rt.readIntField(v, fl, "mBottom", 0);
    p.scrollX = rt.readIntField(v, fl, "mScrollX", 0);
    p.scrollY = rt.readIntField(v, fl, "mScrollY", 0);

    // Alpha & translation (if mTransformationInfo is populated)
    p.alpha = 1.0f;
    uint64_t ti = rt.readRefField(v, fl, "mTransformationInfo");
    if (ti >= 0x1000) {
        uint64_t tkl = rt.mem().u32(ti + rt.layout().klass_off);
        if (tkl >= 0x1000) {
            const auto& tfl = rt.fieldLayout(tkl);
            if (tfl.byName.count("mAlpha")) {
                float a = rt.readFloatField(ti, tfl, "mAlpha", 1.0f);
                if (std::isfinite(a) && a >= 0.0f && a <= 1.0f) p.alpha = a;
            }
            if (tfl.byName.count("mTranslationX")) {
                float tx = rt.readFloatField(ti, tfl, "mTranslationX", 0.0f);
                if (std::isfinite(tx)) p.transX = static_cast<int32_t>(std::lround(tx));
            }
            if (tfl.byName.count("mTranslationY")) {
                float ty = rt.readFloatField(ti, tfl, "mTranslationY", 0.0f);
                if (std::isfinite(ty)) p.transY = static_cast<int32_t>(std::lround(ty));
            }
        }
    }

    p.parent = rt.readRefField(v, fl, "mParent");
    p.childrenArr = rt.readRefField(v, fl, "mChildren");
    p.childrenCount = rt.readIntField(v, fl, "mChildrenCount", 0);

    bool hasScrollbars = (p.viewFlags & 0x00000300) != 0;
    p.scrollable = isScrollableViewClass(rt, p.klass, p.className, p.superClasses) ||
                   p.scrollX != 0 || p.scrollY != 0 ||
                   (hasScrollbars && p.childrenCount > 0);

    if (readStrings) {
        if (uint64_t t = rt.readRefField(v, fl, "mText"); t >= 0x1000) {
            p.text = rt.readCharSeq(t);
        }
        if (uint64_t d = rt.readRefField(v, fl, "mContentDescription"); d >= 0x1000) {
            p.desc = rt.readCharSeq(d);
        }
        if (uint64_t tg = rt.readRefField(v, fl, "mTag"); tg >= 0x1000) {
            p.tag = rt.readCharSeq(tg);
        }
    }
    return p;
}

std::string visibilityString(int32_t visBits) {
    if (visBits == 4) return "INVISIBLE";
    if (visBits == 8) return "GONE";
    return "VISIBLE";
}

struct OffscreenResult {
    bool inViewport = false;
    double visibleAreaRatio = 0.0;
    std::string direction = "hidden";
};

OffscreenResult computeOffscreen(const Rect& screenBounds,
                                 const Rect& visibleBounds,
                                 const Rect& viewportBounds,
                                 bool effectiveVisible) {
    OffscreenResult res;
    int64_t screenArea = screenBounds.area();
    if (!effectiveVisible || screenArea <= 0) {
        res.inViewport = false;
        res.visibleAreaRatio = 0.0;
        res.direction = "hidden";
        return res;
    }

    Rect visInVp = visibleBounds.intersect(viewportBounds);
    int32_t vw = visInVp.width();
    int32_t vh = visInVp.height();
    res.visibleAreaRatio = static_cast<double>(visInVp.area()) / static_cast<double>(screenArea);
    // Round to 4 decimal places for clean output while keeping accuracy
    if (res.visibleAreaRatio >= 0.15 && vw >= 8 && vh >= 8) {
        res.inViewport = true;
        res.direction = "none";
        return res;
    }

    res.inViewport = false;
    int32_t centerX = (screenBounds.left + screenBounds.right) / 2;
    int32_t centerY = (screenBounds.top + screenBounds.bottom) / 2;

    if (screenBounds.bottom <= viewportBounds.top + 8 || centerY < viewportBounds.top) {
        res.direction = "above";
    } else if (screenBounds.top >= viewportBounds.bottom - 8 || centerY > viewportBounds.bottom) {
        res.direction = "below";
    } else if (screenBounds.right <= viewportBounds.left + 8 || centerX < viewportBounds.left) {
        res.direction = "left";
    } else if (screenBounds.left >= viewportBounds.right - 8 || centerX > viewportBounds.right) {
        res.direction = "right";
    } else {
        res.direction = "hidden";
    }
    return res;
}

Rect inferScreenRect(const ViewRootInfo& vri, const std::vector<ViewRootInfo>& allRoots) {
    if (!vri.dispRect.empty()) {
        return Rect{0, 0, vri.dispRect.right, vri.dispRect.bottom};
    }
    for (const auto& r : allRoots) {
        if (!r.dispRect.empty()) {
            return Rect{0, 0, r.dispRect.right, r.dispRect.bottom};
        }
    }
    int32_t maxW = std::max(vri.winFrame.right, vri.width);
    int32_t maxH = std::max(vri.winFrame.bottom, vri.height);
    for (const auto& r : allRoots) {
        maxW = std::max({maxW, r.winFrame.right, r.width});
        maxH = std::max({maxH, r.winFrame.bottom, r.height});
    }
    if (maxW <= 0) maxW = 1080;
    if (maxH <= 0) maxH = 2400;
    return Rect{0, 0, maxW, maxH};
}

ViewRootInfo readOneViewRoot(ArtRuntime& rt, uint64_t vriAddr, bool countNodes) {
    ViewRootInfo info;
    info.addr = vriAddr;
    uint64_t vriKlass = rt.mem().u32(vriAddr + rt.layout().klass_off);
    const auto& l = rt.fieldLayout(vriKlass);

    info.viewAddr = rt.readRefField(vriAddr, l, "mView");
    if (info.viewAddr >= 0x1000) {
        uint64_t vkl = rt.mem().u32(info.viewAddr + rt.layout().klass_off);
        info.viewClass = rt.dottedName(vkl);
    }
    info.width = rt.readIntField(vriAddr, l, "mWidth", 0);
    info.height = rt.readIntField(vriAddr, l, "mHeight", 0);
    info.added = rt.readBoolField(vriAddr, l, "mAdded", false);

    if (uint64_t ai = rt.readRefField(vriAddr, l, "mAttachInfo"); ai >= 0x1000) {
        uint64_t aiKl = rt.mem().u32(ai + rt.layout().klass_off);
        const auto& aiL = rt.fieldLayout(aiKl);
        info.hasWindowFocus = rt.readBoolField(ai, aiL, "mHasWindowFocus", false);
        info.windowVisibility = rt.readIntField(ai, aiL, "mWindowVisibility", 0);
    }

    // Read window frame (prefer mWinFrameInScreen if populated, else mWinFrame)
    Rect wfInScreen = readRectObj(rt, rt.readRefField(vriAddr, l, "mWinFrameInScreen"));
    Rect wf = readRectObj(rt, rt.readRefField(vriAddr, l, "mWinFrame"));
    if (!wfInScreen.empty()) {
        info.winFrame = wfInScreen;
    } else if (!wf.empty()) {
        info.winFrame = wf;
    } else {
        info.winFrame = Rect{0, 0, info.width, info.height};
    }

    info.dispRect = readRectObj(rt, rt.readRefField(vriAddr, l, "mDispRect"));

    // Resolve owning Activity from Context (ViewRootImpl.mContext -> DecorView.mWindow.mContext -> DecorView.mContext)
    uint64_t act = unwrapActivityContext(rt, rt.readRefField(vriAddr, l, "mContext"));
    if (act == 0 && info.viewAddr >= 0x1000) {
        uint64_t vkl = rt.mem().u32(info.viewAddr + rt.layout().klass_off);
        const auto& vl = rt.fieldLayout(vkl);
        if (uint64_t win = rt.readRefField(info.viewAddr, vl, "mWindow"); win >= 0x1000) {
            uint64_t wkl = rt.mem().u32(win + rt.layout().klass_off);
            const auto& wl = rt.fieldLayout(wkl);
            act = unwrapActivityContext(rt, rt.readRefField(win, wl, "mContext"));
        }
        if (act == 0) {
            act = unwrapActivityContext(rt, rt.readRefField(info.viewAddr, vl, "mContext"));
        }
    }
    if (act >= 0x1000) {
        info.activityAddr = act;
        info.activityClass = rt.dottedName(rt.mem().u32(act + rt.layout().klass_off));
    }

    if (countNodes && info.viewAddr >= 0x1000) {
        std::unordered_set<uint64_t> seen;
        info.nodeCount = countViewSubtree(rt, info.viewAddr, seen, 0);
    }
    return info;
}

Json walkTreeNode(ArtRuntime& rt, uint64_t v,
                  int32_t parentOriginX, int32_t parentOriginY,
                  const Rect& parentClip, const Rect& viewportRect,
                  bool parentEffVis, int depth, int maxDepth,
                  std::unordered_set<uint64_t>& visited) {
    if (v < 0x1000 || (v & 7) != 0 || depth > maxDepth || !visited.insert(v).second) {
        return Json(nullptr);
    }
    BasicViewProps p = readBasicViewProps(rt, v, true);
    if (p.klass < 0x1000) return Json(nullptr);

    Rect localBounds{p.left, p.top, p.right, p.bottom};
    int32_t sx = parentOriginX + p.left + p.transX;
    int32_t sy = parentOriginY + p.top + p.transY;
    Rect screenBounds{sx, sy, sx + (p.right - p.left), sy + (p.bottom - p.top)};
    bool effVis = parentEffVis && (p.visBits == 0) && (p.alpha > 0.0f);
    Rect visibleBounds = effVis ? screenBounds.intersect(parentClip) : Rect{0, 0, 0, 0};

    Rect childViewport = p.scrollable && !visibleBounds.empty() ? visibleBounds : viewportRect;
    OffscreenResult off = computeOffscreen(screenBounds, visibleBounds, viewportRect, effVis);

    Json node = Json::object();
    node.set("addr", hexAddr(p.addr));
    node.set("className", p.className);
    Json supers = Json::array();
    for (const auto& s : p.superClasses) supers.push(s);
    node.set("superClasses", std::move(supers));
    node.set("a11yViewId", p.a11yViewId);
    node.set("resourceIdInt", p.resourceIdInt);
    node.set("visibility", visibilityString(p.visBits));
    node.set("effectiveVisible", effVis);
    node.set("clickable", p.clickable);
    node.set("scrollable", p.scrollable);
    node.set("enabled", p.enabled);
    node.set("text", p.text);
    node.set("desc", p.desc);
    node.set("tag", p.tag);
    node.set("localBounds", localBounds.toJson());
    node.set("screenBounds", screenBounds.toJson());
    node.set("visibleBounds", visibleBounds.toJson());
    node.set("inViewport", off.inViewport);
    node.set("visibleAreaRatio", off.visibleAreaRatio);

    Json kids = Json::array();
    if (p.childrenArr >= 0x1000 && p.childrenCount > 0 && depth < maxDepth) {
        int32_t alen = rt.mem().i32(p.childrenArr + 8);
        int32_t lim = std::min({p.childrenCount, alen, 4000});
        int32_t childOriginX = sx - p.scrollX;
        int32_t childOriginY = sy - p.scrollY;
        Rect childClip = visibleBounds.empty() ? Rect{0, 0, 0, 0} : visibleBounds;
        for (int32_t i = 0; i < lim; ++i) {
            uint64_t c = rt.mem().u32(p.childrenArr + 12 + static_cast<uint64_t>(i) * 4);
            if (c < 0x1000) continue;
            Json cj = walkTreeNode(rt, c, childOriginX, childOriginY, childClip,
                                   childViewport, effVis, depth + 1, maxDepth, visited);
            if (!cj.isNull()) kids.push(std::move(cj));
        }
    }
    node.set("children", std::move(kids));
    return node;
}

} // namespace

uint64_t unwrapActivityContext(ArtRuntime& rt, uint64_t ctxAddr) {
    std::unordered_set<uint64_t> seen;
    uint64_t cur = ctxAddr;
    for (int hop = 0; cur >= 0x1000 && hop < 16 && seen.insert(cur).second; ++hop) {
        uint64_t kl = rt.mem().u32(cur + rt.layout().klass_off);
        if (kl < 0x1000) break;
        if (rt.isActivityClass(kl)) return cur;
        const auto& fl = rt.fieldLayout(kl);
        uint64_t next = rt.readRefField(cur, fl, "mBase");
        if (next < 0x1000) next = rt.readRefField(cur, fl, "mOuterContext");
        cur = next;
    }
    return 0;
}

std::vector<ViewRootInfo> findViewRoots(ArtRuntime& rt, bool countNodes) {
    std::vector<ViewRootInfo> out;
    std::unordered_set<uint64_t> seen;
    uint64_t vriClass = rt.viewRootImplClass();
    if (vriClass == 0) return out;
    const auto& l = rt.fieldLayout(vriClass);

    auto tryAddRoot = [&](uint64_t addr) {
        if (addr < 0x1000 || (addr & 7) != 0 || !seen.insert(addr).second) return;
        uint64_t kl = rt.mem().u32(addr + rt.layout().klass_off);
        if (kl != vriClass && !rt.isInstanceOf(kl, "Landroid/view/ViewRootImpl;")) return;
        uint64_t mv = rt.readRefField(addr, l, "mView");
        if (mv < 0x1000) return;
        uint64_t mvKl = rt.mem().u32(mv + rt.layout().klass_off);
        if (!rt.isViewClass(mvKl)) return;
        out.push_back(readOneViewRoot(rt, addr, countNodes));
    };

    // 1) O(1) Fast Path: WindowManagerGlobal.sDefaultWindowManager.mRoots (ArrayList<ViewRootImpl>)
    if (uint64_t wmgClass = rt.findClass("android.view.WindowManagerGlobal"); wmgClass != 0) {
        const auto& sfl = rt.staticFieldLayout(wmgClass);
        auto it = sfl.byName.find("sDefaultWindowManager");
        if (it != sfl.byName.end()) {
            uint64_t wmg = rt.mem().u32(wmgClass + it->second.offset);
            if (wmg >= 0x1000) {
                const auto& wfl = rt.fieldLayout(rt.mem().u32(wmg + rt.layout().klass_off));
                uint64_t mRoots = rt.readRefField(wmg, wfl, "mRoots");
                if (mRoots >= 0x1000) {
                    std::vector<FieldVal> elems;
                    if (unpackList(rt, mRoots, elems, 128)) {
                        for (const auto& fv : elems) {
                            if (fv.isRef) tryAddRoot(fv.ref);
                        }
                    }
                }
            }
        }
    }

    // 2) Fallback: heap scan for ViewRootImpl instances if WindowManagerGlobal yielded nothing
    if (out.empty()) {
        auto insts = rt.scanInstances({vriClass}, 128);
        for (const auto& [addr, _] : insts) {
            tryAddRoot(addr);
        }
    }

    // Sort focused + added roots first
    std::stable_sort(out.begin(), out.end(), [](const ViewRootInfo& a, const ViewRootInfo& b) {
        int sa = (a.added ? 2 : 0) + (a.hasWindowFocus ? 4 : 0) + (a.windowVisibility == 0 ? 1 : 0);
        int sb = (b.added ? 2 : 0) + (b.hasWindowFocus ? 4 : 0) + (b.windowVisibility == 0 ? 1 : 0);
        if (sa != sb) return sa > sb;
        return a.nodeCount > b.nodeCount;
    });
    return out;
}

static uint64_t findActivityThreadInstance(ArtRuntime& rt) {
    uint64_t atClass = rt.activityThreadClass();
    if (atClass == 0) return 0;

    // 1) Try static field ActivityThread.sCurrentActivityThread (O(1)!)
    const auto& sfl = rt.staticFieldLayout(atClass);
    auto it = sfl.byName.find("sCurrentActivityThread");
    if (it != sfl.byName.end()) {
        uint64_t inst = rt.mem().u32(atClass + it->second.offset);
        if (inst >= 0x1000 && rt.mem().u32(inst + rt.layout().klass_off) == static_cast<uint32_t>(atClass)) {
            return inst;
        }
    }

    // 2) Fallback: scan heap for ActivityThread singleton instance
    auto insts = rt.scanInstances({atClass}, 4);
    if (!insts.empty()) return insts.front().first;
    return 0;
}

std::vector<ActivityRecordInfo> findActivities(ArtRuntime& rt, const std::vector<ViewRootInfo>* knownRoots) {
    std::vector<ActivityRecordInfo> out;
    std::unordered_set<uint64_t> seenAct;

    uint64_t at = findActivityThreadInstance(rt);
    if (at >= 0x1000) {
        uint64_t atKl = rt.mem().u32(at + rt.layout().klass_off);
        const auto& atFl = rt.fieldLayout(atKl);
        uint64_t mActivities = rt.readRefField(at, atFl, "mActivities");
        if (mActivities >= 0x1000) {
            std::vector<std::pair<std::string, FieldVal>> entries;
            if (unpackMap(rt, mActivities, entries, 256)) {
                for (const auto& [_, fv] : entries) {
                    if (!fv.isRef || fv.ref < 0x1000) continue;
                    uint64_t rec = fv.ref;
                    uint64_t recKl = rt.mem().u32(rec + rt.layout().klass_off);
                    if (recKl < 0x1000) continue;
                    const auto& recFl = rt.fieldLayout(recKl);

                    uint64_t act = rt.readRefField(rec, recFl, "activity");
                    if (act < 0x1000 || !seenAct.insert(act).second) continue;

                    uint64_t actKl = rt.mem().u32(act + rt.layout().klass_off);
                    const auto& actFl = rt.fieldLayout(actKl);

                    bool recPaused = rt.readBoolField(rec, recFl, "paused", false);
                    bool recStopped = rt.readBoolField(rec, recFl, "stopped", false);

                    bool hasMResumed = actFl.byName.count("mResumed") > 0;
                    bool mResumed = hasMResumed && rt.readBoolField(act, actFl, "mResumed", false);
                    bool mStopped = rt.readBoolField(act, actFl, "mStopped", false);
                    bool mDestroyed = rt.readBoolField(act, actFl, "mDestroyed", false);
                    bool mFinished = rt.readBoolField(act, actFl, "mFinished", false);

                    ActivityRecordInfo info;
                    info.addr = act;
                    info.className = rt.dottedName(actKl);
                    info.destroyed = mDestroyed || mFinished;
                    info.stopped = recStopped || mStopped;
                    info.paused = recPaused || (hasMResumed && !mResumed);
                    info.resumed = hasMResumed ? (mResumed && !info.destroyed)
                                               : (!info.paused && !info.stopped && !info.destroyed);
                    out.push_back(std::move(info));
                }
            }
        }
    }

    // Cross-check with ViewRootImpl contexts in case an Activity wasn't in mActivities
    std::vector<ViewRootInfo> localRoots;
    const std::vector<ViewRootInfo>* rootsPtr = knownRoots;
    if (!rootsPtr && out.empty()) {
        localRoots = findViewRoots(rt, false);
        rootsPtr = &localRoots;
    }
    if (rootsPtr) {
        for (const auto& vr : *rootsPtr) {
            if (vr.activityAddr >= 0x1000 && seenAct.insert(vr.activityAddr).second) {
                uint64_t actKl = rt.mem().u32(vr.activityAddr + rt.layout().klass_off);
                const auto& actFl = rt.fieldLayout(actKl);
                bool mResumed = rt.readBoolField(vr.activityAddr, actFl, "mResumed", vr.hasWindowFocus);
                bool mStopped = rt.readBoolField(vr.activityAddr, actFl, "mStopped", !vr.added);
                bool mDestroyed = rt.readBoolField(vr.activityAddr, actFl, "mDestroyed", false);
                ActivityRecordInfo info;
                info.addr = vr.activityAddr;
                info.className = vr.activityClass;
                info.destroyed = mDestroyed;
                info.stopped = mStopped;
                info.resumed = mResumed && !mDestroyed;
                info.paused = !info.resumed;
                out.push_back(std::move(info));
            }
        }
    }

    // Sort resumed & non-paused activities first
    std::stable_sort(out.begin(), out.end(), [](const ActivityRecordInfo& a, const ActivityRecordInfo& b) {
        int sa = (a.resumed ? 4 : 0) + (!a.paused ? 2 : 0) + (!a.stopped ? 1 : 0) - (a.destroyed ? 8 : 0);
        int sb = (b.resumed ? 4 : 0) + (!b.paused ? 2 : 0) + (!b.stopped ? 1 : 0) - (b.destroyed ? 8 : 0);
        return sa > sb;
    });
    return out;
}

uint64_t findCurrentActivity(ArtRuntime& rt) {
    auto roots = findViewRoots(rt, false);
    auto acts = findActivities(rt, &roots);

    // 1) Cross-check focused ViewRootImpl's unwrapped Activity with live mActivities
    uint64_t focusedAct = 0;
    for (const auto& vr : roots) {
        if (vr.added && vr.hasWindowFocus && vr.windowVisibility == 0 && vr.activityAddr >= 0x1000) {
            focusedAct = vr.activityAddr;
            break;
        }
    }
    if (focusedAct != 0) {
        for (const auto& a : acts) {
            if (a.addr == focusedAct && !a.destroyed) {
                return a.addr;
            }
        }
    }
    // 2) First resumed & non-paused activity in ActivityThread.mActivities
    for (const auto& a : acts) {
        if (a.resumed && !a.paused && !a.destroyed) return a.addr;
    }
    // 3) Fallback to focused ViewRootImpl activity
    if (focusedAct != 0) return focusedAct;
    // 4) First non-destroyed activity
    for (const auto& a : acts) {
        if (!a.destroyed) return a.addr;
    }
    if (!acts.empty()) return acts.front().addr;
    return 0;
}

Json opRoots(ArtRuntime& rt) {
    auto viewRoots = findViewRoots(rt, true);
    auto activities = findActivities(rt, &viewRoots);

    Json actArr = Json::array();
    for (const auto& a : activities) actArr.push(a.toJson());
    Json vrArr = Json::array();
    for (const auto& vr : viewRoots) vrArr.push(vr.toJson());

    return Json::object({
        {"activities", std::move(actArr)},
        {"viewRoots", std::move(vrArr)}
    });
}

Json opTree(ArtRuntime& rt, bool onlyFocused, uint64_t filterRootAddr, int maxDepth) {
    auto allRoots = findViewRoots(rt, false);
    Json rootsJson = Json::array();

    for (const auto& vr : allRoots) {
        if (filterRootAddr != 0 && vr.addr != filterRootAddr && vr.viewAddr != filterRootAddr) {
            continue;
        }
        if (onlyFocused && !(vr.added && vr.hasWindowFocus)) {
            continue;
        }
        Rect screenRect = inferScreenRect(vr, allRoots);
        std::unordered_set<uint64_t> visited;
        Json treeNode = walkTreeNode(rt, vr.viewAddr,
                                     vr.winFrame.left, vr.winFrame.top,
                                     screenRect, screenRect,
                                     vr.windowVisibility == 0,
                                     0, maxDepth, visited);

        Json entry = Json::object();
        entry.set("rootAddr", hexAddr(vr.addr));
        entry.set("activityClass", !vr.activityClass.empty() ? Json(vr.activityClass) : Json(nullptr));
        entry.set("hasWindowFocus", vr.hasWindowFocus);
        entry.set("winFrame", vr.winFrame.toJson());
        entry.set("tree", std::move(treeNode));
        rootsJson.push(std::move(entry));
    }

    return Json::object({{"roots", std::move(rootsJson)}});
}

uint64_t findViewByFilter(ArtRuntime& rt, const ViewFilter& filter) {
    auto allRoots = findViewRoots(rt, false);
    if (allRoots.empty()) return 0;

    uint64_t bestAddr = 0;
    double bestScore = -1e18;
    bool exactA11yFound = false;

    for (const auto& vr : allRoots) {
        if (filter.onlyFocused && !(vr.added && vr.hasWindowFocus)) continue;
        bool isFocused = vr.added && vr.hasWindowFocus && vr.windowVisibility == 0;
        Rect screenRect = inferScreenRect(vr, allRoots);
        std::unordered_set<uint64_t> visited;

        auto dfs = [&](auto& self, uint64_t v, int32_t ox, int32_t oy,
                       const Rect& clip, bool parentEffVis, int depth) -> void {
            if (exactA11yFound) return;
            if (v < 0x1000 || (v & 7) != 0 || depth > 128 || !visited.insert(v).second) return;

            bool needStrings = filter.hasText || filter.hasDesc;
            BasicViewProps p = readBasicViewProps(rt, v, needStrings);
            if (p.klass < 0x1000) return;

            // 1) Exact O(1)-style match on a11yViewId > 0!
            if (filter.hasA11yViewId && filter.a11yViewId > 0) {
                if (p.a11yViewId == filter.a11yViewId) {
                    bestAddr = v;
                    exactA11yFound = true;
                    return;
                }
            } else {
                // Scoring match when a11yViewId is absent
                int32_t sx = ox + p.left + p.transX;
                int32_t sy = oy + p.top + p.transY;
                Rect sb{sx, sy, sx + (p.right - p.left), sy + (p.bottom - p.top)};
                bool effVis = parentEffVis && (p.visBits == 0) && (p.alpha > 0.0f);

                bool ok = true;
                double score = 0.0;

                if (filter.hasResourceIdInt) {
                    if (p.resourceIdInt == filter.resourceIdInt) score += 1000.0;
                    else ok = false;
                }
                if (ok && filter.hasClassName) {
                    auto descOpt = rt.descriptor(p.klass);
                    bool cm = (descOpt && typeMatchesPattern(*descOpt, filter.className)) ||
                              (shortClassName(p.className) == filter.className);
                    if (cm) score += 500.0;
                    else ok = false;
                }
                if (ok && filter.hasText) {
                    if (p.text == filter.text) score += 1200.0;
                    else if (!filter.text.empty() && p.text.find(filter.text) != std::string::npos) score += 450.0;
                    else ok = false;
                }
                if (ok && filter.hasDesc) {
                    if (p.desc == filter.desc) score += 1200.0;
                    else if (!filter.desc.empty() && p.desc.find(filter.desc) != std::string::npos) score += 450.0;
                    else ok = false;
                }
                if (ok && filter.bounds) {
                    const Rect& want = *filter.bounds;
                    int32_t l1 = std::abs(sb.left - want.left) + std::abs(sb.top - want.top) +
                                 std::abs(sb.right - want.right) + std::abs(sb.bottom - want.bottom);
                    if (l1 == 0) {
                        score += 2000.0;
                    } else {
                        Rect inter = sb.intersect(want);
                        int64_t interArea = inter.area();
                        int64_t unionArea = sb.area() + want.area() - interArea;
                        double iou = (unionArea > 0) ? (static_cast<double>(interArea) / static_cast<double>(unionArea)) : 0.0;
                        if (iou >= 0.3 || l1 <= 64) {
                            score += iou * 1000.0 - static_cast<double>(l1) * 0.5;
                        } else {
                            ok = false;
                        }
                    }
                }

                if (ok) {
                    if (effVis) score += 100.0;
                    if (isFocused) score += 50.0;
                    score += static_cast<double>(depth) * 0.5; // prefer most specific child
                    if (score > bestScore) {
                        bestScore = score;
                        bestAddr = v;
                    }
                }
            }

            if (p.childrenArr >= 0x1000 && p.childrenCount > 0) {
                int32_t sx = ox + p.left + p.transX;
                int32_t sy = oy + p.top + p.transY;
                Rect sb{sx, sy, sx + (p.right - p.left), sy + (p.bottom - p.top)};
                bool effVis = parentEffVis && (p.visBits == 0) && (p.alpha > 0.0f);
                Rect vb = effVis ? sb.intersect(clip) : Rect{0, 0, 0, 0};
                int32_t alen = rt.mem().i32(p.childrenArr + 8);
                int32_t lim = std::min({p.childrenCount, alen, 4000});
                for (int32_t i = 0; i < lim && !exactA11yFound; ++i) {
                    uint64_t c = rt.mem().u32(p.childrenArr + 12 + static_cast<uint64_t>(i) * 4);
                    if (c >= 0x1000) {
                        self(self, c, sx - p.scrollX, sy - p.scrollY, vb, effVis, depth + 1);
                    }
                }
            }
        };

        dfs(dfs, vr.viewAddr, vr.winFrame.left, vr.winFrame.top, screenRect, vr.windowVisibility == 0, 0);
        if (exactA11yFound) break;
    }

    return bestAddr;
}

Json computeViewMeta(ArtRuntime& rt, uint64_t viewAddr) {
    if (viewAddr < 0x1000 || (viewAddr & 7) != 0) return Json(nullptr);
    uint64_t kl = rt.mem().u32(viewAddr + rt.layout().klass_off);
    if (!rt.isViewClass(kl)) return Json(nullptr);

    // 1) Walk mParent upward to collect ancestor chain [view, p1, p2, ..., decorView] and detect ViewRootImpl
    std::vector<BasicViewProps> chain; // chain[0] = target view, chain.back() = root View
    std::unordered_set<uint64_t> seen;
    uint64_t detectedVri = 0;
    uint64_t vriClass = rt.viewRootImplClass();

    uint64_t cur = viewAddr;
    for (int hop = 0; cur >= 0x1000 && hop < 128 && seen.insert(cur).second; ++hop) {
        uint64_t ckl = rt.mem().u32(cur + rt.layout().klass_off);
        if (ckl < 0x1000) break;
        if (vriClass != 0 && ckl == vriClass) {
            detectedVri = cur;
            break;
        }
        if (!rt.isViewClass(ckl)) {
            if (rt.dottedName(ckl) == "android.view.ViewRootImpl") {
                detectedVri = cur;
            }
            break;
        }
        BasicViewProps p = readBasicViewProps(rt, cur, hop == 0);
        uint64_t nextParent = p.parent;
        chain.push_back(std::move(p));
        cur = nextParent;
    }
    if (chain.empty()) return Json(nullptr);

    // 2) Resolve owning ViewRootImpl & screen dimensions
    auto allRoots = findViewRoots(rt, false);
    ViewRootInfo owningRoot;
    bool hasRoot = false;
    if (detectedVri != 0) {
        for (const auto& r : allRoots) {
            if (r.addr == detectedVri) {
                owningRoot = r;
                hasRoot = true;
                break;
            }
        }
        if (!hasRoot) {
            owningRoot = readOneViewRoot(rt, detectedVri, false);
            hasRoot = true;
        }
    }
    if (!hasRoot) {
        uint64_t topView = chain.back().addr;
        for (const auto& r : allRoots) {
            if (r.viewAddr == topView) {
                owningRoot = r;
                hasRoot = true;
                break;
            }
        }
    }

    Rect screenRect = hasRoot ? inferScreenRect(owningRoot, allRoots)
                              : (!allRoots.empty() ? inferScreenRect(allRoots.front(), allRoots)
                                                   : Rect{0, 0, 1080, 2400});
    Rect winFrame = hasRoot ? owningRoot.winFrame : screenRect;
    bool isFocusedWindow = hasRoot ? (owningRoot.added && owningRoot.hasWindowFocus && owningRoot.windowVisibility == 0)
                                   : false;
    bool rootWinVis = hasRoot ? (owningRoot.windowVisibility == 0) : true;

    // 3) Compute screenBounds, visibleBounds, effectiveVisible top-down from chain.back() -> chain[0]
    size_t n = chain.size();
    std::vector<Rect> screenBoundsVec(n);
    std::vector<Rect> visibleBoundsVec(n);
    std::vector<bool> effVisVec(n, false);

    int32_t originX = winFrame.left;
    int32_t originY = winFrame.top;
    Rect curClip = screenRect;
    bool curEffVis = rootWinVis;

    for (size_t rev = 0; rev < n; ++rev) {
        size_t idx = n - 1 - rev;
        const auto& p = chain[idx];
        int32_t sx = originX + p.left + p.transX;
        int32_t sy = originY + p.top + p.transY;
        Rect sb{sx, sy, sx + (p.right - p.left), sy + (p.bottom - p.top)};
        bool ev = curEffVis && (p.visBits == 0) && (p.alpha > 0.0f);
        Rect vb = ev ? sb.intersect(curClip) : Rect{0, 0, 0, 0};

        screenBoundsVec[idx] = sb;
        visibleBoundsVec[idx] = vb;
        effVisVec[idx] = ev;

        originX = sx - p.scrollX;
        originY = sy - p.scrollY;
        curClip = vb;
        curEffVis = ev;
    }

    // 4) Find nearest scrollable ancestor (idx = 1 .. n-1) & build parentChain
    Json scrollableAnc = Json(nullptr);
    Rect viewportBounds = screenRect;
    Json parentChain = Json::array();

    for (size_t idx = 1; idx < n; ++idx) {
        const auto& anc = chain[idx];
        parentChain.push(Json::object({
            {"addr", hexAddr(anc.addr)},
            {"className", anc.className},
            {"a11yViewId", anc.a11yViewId},
            {"resourceIdInt", anc.resourceIdInt}
        }));
        if (scrollableAnc.isNull() && anc.scrollable) {
            scrollableAnc = Json::object({
                {"addr", hexAddr(anc.addr)},
                {"a11yViewId", anc.a11yViewId},
                {"className", anc.className},
                {"visibleBounds", visibleBoundsVec[idx].toJson()}
            });
            if (!visibleBoundsVec[idx].empty()) {
                viewportBounds = visibleBoundsVec[idx].intersect(screenRect);
            }
        }
    }

    const auto& self = chain[0];
    OffscreenResult off = computeOffscreen(screenBoundsVec[0], visibleBoundsVec[0],
                                           viewportBounds, effVisVec[0]);

    Json supers = Json::array();
    for (const auto& s : self.superClasses) supers.push(s);

    Json out = Json::object();
    out.set("addr", hexAddr(self.addr));
    out.set("className", self.className);
    out.set("superClasses", std::move(supers));
    out.set("a11yViewId", self.a11yViewId);
    out.set("resourceIdInt", self.resourceIdInt);
    out.set("visibility", visibilityString(self.visBits));
    out.set("effectiveVisible", static_cast<bool>(effVisVec[0]));
    out.set("alpha", static_cast<double>(self.alpha));
    out.set("clickable", self.clickable);
    out.set("scrollable", self.scrollable);
    out.set("enabled", self.enabled);
    out.set("text", self.text);
    out.set("desc", self.desc);
    out.set("screenBounds", screenBoundsVec[0].toJson());
    out.set("visibleBounds", visibleBoundsVec[0].toJson());
    out.set("viewportBounds", viewportBounds.toJson());
    out.set("screenSize", Json::object({
        {"width", screenRect.width()},
        {"height", screenRect.height()}
    }));
    out.set("inViewport", off.inViewport);
    out.set("visibleAreaRatio", off.visibleAreaRatio);
    out.set("offscreenDirection", off.direction);
    out.set("isFocusedWindow", isFocusedWindow);
    out.set("scrollableAncestor", std::move(scrollableAnc));
    out.set("parentChain", std::move(parentChain));
    return out;
}

} // namespace artseek
