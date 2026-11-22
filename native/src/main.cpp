#include "art.hpp"
#include "calibrate.hpp"
#include "freezer.hpp"
#include "json.hpp"
#include "layout.hpp"
#include "mem.hpp"
#include "query.hpp"
#include "view.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace artseek {

static constexpr const char* kVersion = "0.1.1";
static constexpr const char* kDefaultCacheDir = "/data/local/tmp/.artseek";

struct ServerState {
    std::string cacheDir = kDefaultCacheDir;
    std::string layoutPath;
    bool layoutLoaded = false;
    ArtLayout layout;
    RuntimeManager rtMgr;

    explicit ServerState(std::string dir)
        : cacheDir(std::move(dir)),
          layoutPath(cacheDir + "/layout.json"),
          rtMgr(layout) {
        std::string err;
        if (loadLayoutFile(layoutPath, layout, &err)) {
            layoutLoaded = true;
            rtMgr.setLayout(layout);
        }
    }

    bool ensureCalibrated(bool force, bool& didCalibrate, std::string* err) {
        if (!force && layoutLoaded) return true;
        std::fprintf(stderr, "[artseek] %s calibrating ART layout via embedded sentinel...\n",
                     force ? "force" : "auto");
        ArtLayout fresh;
        if (!calibrateWithEmbeddedSentinel(cacheDir, fresh, err)) {
            return false;
        }
        layout = std::move(fresh);
        layoutLoaded = true;
        didCalibrate = true;
        rtMgr.setLayout(layout);
        std::fprintf(stderr, "[artseek] calibration succeeded, saved %s\n", layoutPath.c_str());
        return true;
    }

    std::optional<int> resolveTargetPid(const Json& req, std::string* errCode, std::string* errMsg) {
        if (req.has("pid") && req.get("pid").isNumber()) {
            int p = static_cast<int>(req.get("pid").asInt(0));
            if (p <= 0 || !readProcStartTime(p).has_value()) {
                if (errCode) *errCode = "PROCESS_NOT_FOUND";
                if (errMsg) *errMsg = "pid " + std::to_string(p) + " not found";
                return std::nullopt;
            }
            return p;
        }
        if (req.has("pkg") && req.get("pkg").isString() && !req.get("pkg").asString().empty()) {
            std::string pkg = req.get("pkg").asString();
            auto p = findPidByPackage(pkg);
            if (!p) {
                if (errCode) *errCode = "PROCESS_NOT_FOUND";
                if (errMsg) *errMsg = "package '" + pkg + "' not running";
                return std::nullopt;
            }
            return *p;
        }
        if (errCode) *errCode = "INVALID_QUERY";
        if (errMsg) *errMsg = "request requires 'pid' or 'pkg'";
        return std::nullopt;
    }

    Json makeError(const Json& id, const std::string& code, const std::string& msg,
                   int64_t elapsedMs, int pid = 0, uint64_t starttime = 0,
                   int64_t frozenMs = 0, bool cacheHit = false, bool recalibrated = false) {
        Json meta = Json::object();
        if (pid > 0) meta.set("pid", pid);
        if (starttime > 0) meta.set("starttime", starttime);
        meta.set("elapsedMs", elapsedMs);
        if (pid > 0) {
            meta.set("frozenMs", frozenMs);
            meta.set("cacheHit", cacheHit);
            meta.set("recalibrated", recalibrated);
        }
        Json resp = Json::object();
        if (!id.isNull()) resp.set("id", id);
        resp.set("ok", false);
        resp.set("meta", std::move(meta));
        resp.set("data", Json(nullptr));
        resp.set("error", Json::object({{"code", code}, {"message", msg}}));
        return resp;
    }

    Json makeOk(const Json& id, Json data, int64_t elapsedMs,
                int pid = 0, uint64_t starttime = 0,
                int64_t frozenMs = 0, bool cacheHit = false, bool recalibrated = false) {
        Json meta = Json::object();
        if (pid > 0) meta.set("pid", pid);
        if (starttime > 0) meta.set("starttime", starttime);
        meta.set("elapsedMs", elapsedMs);
        meta.set("frozenMs", frozenMs);
        meta.set("cacheHit", cacheHit);
        meta.set("recalibrated", recalibrated);

        Json resp = Json::object();
        if (!id.isNull()) resp.set("id", id);
        resp.set("ok", true);
        resp.set("meta", std::move(meta));
        resp.set("data", std::move(data));
        resp.set("error", Json(nullptr));
        return resp;
    }

    Json handleRequest(const Json& req) {
        auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&]() -> int64_t {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                .count();
        };

        Json id = req.has("id") ? req.get("id") : Json(nullptr);
        if (!req.isObject()) {
            return makeError(id, "INVALID_QUERY", "request must be a JSON object", elapsed());
        }
        std::string op = req.get("op").asString();
        if (op.empty()) {
            return makeError(id, "INVALID_QUERY", "missing required string field 'op'", elapsed());
        }
        const Json& params = req.has("params") && req.get("params").isObject()
                                 ? req.get("params")
                                 : Json::object();

        // 1) ping (no target process needed)
        if (op == "ping") {
            Json data = Json::object({
                {"version", kVersion},
                {"cacheDir", cacheDir},
                {"layoutLoaded", layoutLoaded},
                {"layoutPath", layoutPath},
                {"hasEmbeddedSentinel", hasEmbeddedSentinel()}
            });
            return makeOk(id, std::move(data), elapsed());
        }

        // 2) calibrate (no target process needed)
        if (op == "calibrate") {
            bool didCal = false;
            std::string calErr;
            if (!ensureCalibrated(true, didCal, &calErr)) {
                return makeError(id, "CALIBRATION_FAILED", calErr, elapsed());
            }
            int64_t ms = elapsed();
            Json data = Json::object({
                {"layout", layout.toNumericMap()},
                {"layoutPath", layoutPath},
                {"elapsedMs", ms}
            });
            return makeOk(id, std::move(data), ms, 0, 0, 0, false, true);
        }

        // All remaining ops require a target process
        std::string pidErrCode, pidErrMsg;
        auto pidOpt = resolveTargetPid(req, &pidErrCode, &pidErrMsg);
        if (!pidOpt) {
            return makeError(id, pidErrCode, pidErrMsg, elapsed());
        }
        int pid = *pidOpt;
        bool freeze = req.get("freeze").asBool(false);
        bool madvise = req.has("madvise") ? req.get("madvise").asBool(true) : true;
        bool recalibrated = false;

        // 3) selfcheck
        if (op == "selfcheck") {
            bool autoRecal = params.has("autoRecalibrate") ? params.get("autoRecalibrate").asBool(true) : true;
            if (!layoutLoaded && autoRecal) {
                std::string calErr;
                if (!ensureCalibrated(false, recalibrated, &calErr)) {
                    return makeError(id, "CALIBRATION_FAILED", calErr, elapsed(), pid);
                }
            }

            std::string acqErr;
            auto h = rtMgr.acquire(pid, &acqErr);
            if (!h) {
                return makeError(id, "PROCESS_NOT_FOUND", acqErr, elapsed(), pid);
            }
            int64_t frozenMs = 0;
            SelfCheckReport rep;
            {
                ThreadFreezer freezer(pid, freeze, &frozenMs);
                rep = runSelfCheck(*h->rt, pid);
            }
            h->rt->endRequest(madvise);

            if (!rep.passed && autoRecal && !recalibrated) {
                std::fprintf(stderr, "[artseek] selfcheck failed, auto-recalibrating...\n");
                std::string calErr;
                if (ensureCalibrated(true, recalibrated, &calErr)) {
                    auto h2 = rtMgr.acquire(pid, &acqErr);
                    if (h2) {
                        int64_t frozenMs2 = 0;
                        {
                            ThreadFreezer freezer(pid, freeze, &frozenMs2);
                            rep = runSelfCheck(*h2->rt, pid);
                        }
                        frozenMs += frozenMs2;
                        h2->rt->endRequest(madvise);
                        h = h2;
                    }
                }
            }
            return makeOk(id, rep.toJson(recalibrated, layout), elapsed(),
                          pid, h->starttime, frozenMs, h->cacheHit, recalibrated);
        }

        // 4) Target ops: roots, tree, strsearch, dumpobj, whorefs, instances, query, batch
        if (!layoutLoaded) {
            std::string calErr;
            if (!ensureCalibrated(false, recalibrated, &calErr)) {
                return makeError(id, "CALIBRATION_FAILED", calErr, elapsed(), pid);
            }
        }

        std::string acqErr;
        auto h = rtMgr.acquire(pid, &acqErr);
        if (!h) {
            return makeError(id, "PROCESS_NOT_FOUND", acqErr, elapsed(), pid);
        }

        // Validate layout on cold process attach (or reload after calibration)
        if (!h->cacheHit) {
            std::string valReason;
            if (!h->rt->quickValidate(&valReason)) {
                h->rt->endRequest(false);
                if (!recalibrated) {
                    std::fprintf(stderr, "[artseek] layout validation failed (%s), auto-recalibrating...\n",
                                 valReason.c_str());
                    std::string calErr;
                    if (!ensureCalibrated(true, recalibrated, &calErr)) {
                        return makeError(id, "CALIBRATION_FAILED",
                                         "validation failed (" + valReason + ") and recalibration failed: " + calErr,
                                         elapsed(), pid, h->starttime);
                    }
                    h = rtMgr.acquire(pid, &acqErr);
                    if (!h) {
                        return makeError(id, "PROCESS_NOT_FOUND", acqErr, elapsed(), pid);
                    }
                    if (!h->rt->quickValidate(&valReason)) {
                        h->rt->endRequest(madvise);
                        return makeError(id, "CALIBRATION_FAILED",
                                         "layout still invalid after recalibration: " + valReason,
                                         elapsed(), pid, h->starttime, 0, false, recalibrated);
                    }
                } else {
                    return makeError(id, "CALIBRATION_FAILED",
                                     "layout validation failed: " + valReason,
                                     elapsed(), pid, h->starttime, 0, false, recalibrated);
                }
            }
        }

        int64_t frozenMs = 0;

        if (op == "batch") {
            if (!params.has("queries") || !params.get("queries").isArray()) {
                h->rt->endRequest(madvise);
                return makeError(id, "INVALID_QUERY", "batch requires array 'queries'",
                                 elapsed(), pid, h->starttime, 0, h->cacheHit, recalibrated);
            }
            Json results = Json::array();
            {
                // Single freeze window for ALL sub-queries!
                ThreadFreezer freezer(pid, freeze, &frozenMs);
                for (const Json& sub : params.get("queries").asArray()) {
                    Json item = Json::object();
                    if (sub.has("id")) item.set("id", sub.get("id"));
                    if (!sub.isObject() || !sub.get("op").isString()) {
                        item.set("ok", false);
                        item.set("error", Json::object({{"code", "INVALID_QUERY"}, {"message", "invalid sub-query"}}));
                        results.push(std::move(item));
                        continue;
                    }
                    std::string subOp = sub.get("op").asString();
                    const Json& subParams = sub.has("params") && sub.get("params").isObject()
                                                ? sub.get("params")
                                                : Json::object();
                    OpResult r = dispatchTargetOp(*h->rt, pid, subOp, subParams);
                    item.set("ok", r.ok);
                    if (r.ok) {
                        item.set("data", std::move(r.data));
                    } else {
                        item.set("error", Json::object({{"code", r.errorCode}, {"message", r.errorMessage}}));
                    }
                    results.push(std::move(item));
                }
            }
            h->rt->endRequest(madvise);
            return makeOk(id, Json::object({{"results", std::move(results)}}), elapsed(),
                          pid, h->starttime, frozenMs, h->cacheHit, recalibrated);
        }

        OpResult res;
        {
            ThreadFreezer freezer(pid, freeze, &frozenMs);
            res = dispatchTargetOp(*h->rt, pid, op, params);
        }
        h->rt->endRequest(madvise);

        if (!res.ok) {
            return makeError(id, res.errorCode, res.errorMessage, elapsed(),
                             pid, h->starttime, frozenMs, h->cacheHit, recalibrated);
        }
        return makeOk(id, std::move(res.data), elapsed(),
                      pid, h->starttime, frozenMs, h->cacheHit, recalibrated);
    }
};

} // namespace artseek

static void printUsage() {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  artseek [--cache-dir /data/local/tmp/.artseek] [--idle-ms 0]\n"
                 "  artseek --version\n"
                 "  artseek --calibrate [--cache-dir <dir>]\n"
                 "  artseek --selfcheck [--pid <pid> | --pkg <pkg>] [--cache-dir <dir>]\n");
}

int main(int argc, char** argv) {
    // Ignore SIGPIPE so writing to closed stdout exits cleanly instead of crashing
    ::signal(SIGPIPE, SIG_IGN);

    std::string cacheDir = artseek::kDefaultCacheDir;
    int idleMs = 0;
    bool flagVersion = false;
    bool flagCalibrate = false;
    bool flagSelfcheck = false;
    int cliPid = 0;
    std::string cliPkg;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            flagVersion = true;
        } else if (a == "--calibrate") {
            flagCalibrate = true;
        } else if (a == "--selfcheck") {
            flagSelfcheck = true;
        } else if (a == "--cache-dir" && i + 1 < argc) {
            cacheDir = argv[++i];
        } else if (a == "--idle-ms" && i + 1 < argc) {
            idleMs = std::atoi(argv[++i]);
        } else if (a == "--pid" && i + 1 < argc) {
            cliPid = std::atoi(argv[++i]);
        } else if (a == "--pkg" && i + 1 < argc) {
            cliPkg = argv[++i];
        } else if (a == "--help" || a == "-h") {
            printUsage();
            return 0;
        } else {
            std::fprintf(stderr, "[artseek] unknown argument: %s\n", a.c_str());
            printUsage();
            return 2;
        }
    }

    if (flagVersion) {
        std::printf("artseek %s\n", artseek::kVersion);
        std::fflush(stdout);
        return 0;
    }

    artseek::ServerState server(cacheDir);

    if (flagCalibrate) {
        artseek::Json req = artseek::Json::object({{"op", "calibrate"}});
        artseek::Json resp = server.handleRequest(req);
        std::string out = resp.dump();
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return resp.get("ok").asBool(false) ? 0 : 1;
    }

    if (flagSelfcheck) {
        artseek::Json req = artseek::Json::object({{"op", "selfcheck"}});
        if (cliPid > 0) req.set("pid", cliPid);
        else if (!cliPkg.empty()) req.set("pkg", cliPkg);
        else {
            std::fprintf(stderr, "[artseek] --selfcheck requires --pid <pid> or --pkg <pkg>\n");
            return 2;
        }
        artseek::Json resp = server.handleRequest(req);
        std::string out = resp.dump();
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        bool passed = resp.get("ok").asBool(false) &&
                      resp.get("data").get("passed").asBool(false);
        return passed ? 0 : 1;
    }

    // Default mode: persistent reusable NDJSON stdio loop (also works cleanly for one-shot piped stdin)
    std::string rxBuf;
    rxBuf.reserve(4096);
    bool eof = false;

    auto processLine = [&](std::string_view line) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) return;

        std::string perr;
        auto parsed = artseek::Json::parse(line, &perr);
        artseek::Json resp;
        if (!parsed) {
            resp = server.makeError(artseek::Json(nullptr), "INVALID_QUERY",
                                    "malformed JSON: " + perr, 0);
        } else {
            resp = server.handleRequest(*parsed);
        }
        std::string out = resp.dump();
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };

    while (true) {
        // Drain any complete lines already buffered before waiting on poll()
        size_t nl = rxBuf.find('\n');
        while (nl != std::string::npos) {
            std::string line = rxBuf.substr(0, nl);
            rxBuf.erase(0, nl + 1);
            processLine(line);
            nl = rxBuf.find('\n');
        }
        if (eof) {
            if (!rxBuf.empty()) {
                processLine(rxBuf);
                rxBuf.clear();
            }
            break;
        }

        if (idleMs > 0) {
            struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
            int pr = ::poll(&pfd, 1, idleMs);
            if (pr == 0) {
                std::fprintf(stderr, "[artseek] idle timeout (%dms), exiting\n", idleMs);
                break;
            }
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
        }

        char tmp[4096];
        ssize_t n = ::read(STDIN_FILENO, tmp, sizeof(tmp));
        if (n > 0) {
            rxBuf.append(tmp, static_cast<size_t>(n));
        } else if (n == 0) {
            eof = true;
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }

    return 0;
}
