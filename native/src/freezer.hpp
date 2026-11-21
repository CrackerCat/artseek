#pragma once

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace artseek {

class ThreadFreezer {
public:
    ThreadFreezer(int pid, bool enable, int64_t* outFrozenMs = nullptr)
        : pid_(pid), active_(false), outMs_(outFrozenMs) {
        if (!enable || pid <= 0) {
            if (outMs_) *outMs_ = 0;
            return;
        }
        start_ = std::chrono::steady_clock::now();
        std::string taskDir = "/proc/" + std::to_string(pid_) + "/task";
        DIR* dir = ::opendir(taskDir.c_str());
        if (!dir) {
            if (outMs_) *outMs_ = 0;
            return;
        }
        struct dirent* ent;
        while ((ent = ::readdir(dir)) != nullptr) {
            if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
            int tid = std::atoi(ent->d_name);
            if (tid > 0) {
                tids_.push_back(tid);
            }
        }
        ::closedir(dir);

        if (tids_.empty()) {
            if (outMs_) *outMs_ = 0;
            return;
        }
        active_ = true;
        for (int tid : tids_) {
            ::syscall(SYS_tgkill, pid_, tid, SIGSTOP);
        }
        // Best-effort wait up to 4ms for threads to reach stopped state
        for (int attempt = 0; attempt < 8; ++attempt) {
            bool allStopped = true;
            for (int tid : tids_) {
                if (!isTidStopped(tid)) {
                    allStopped = false;
                    break;
                }
            }
            if (allStopped) break;
            ::usleep(500);
        }
    }

    ~ThreadFreezer() {
        release();
    }

    ThreadFreezer(const ThreadFreezer&) = delete;
    ThreadFreezer& operator=(const ThreadFreezer&) = delete;

    void release() {
        if (!active_) return;
        active_ = false;
        for (int tid : tids_) {
            ::syscall(SYS_tgkill, pid_, tid, SIGCONT);
        }
        ::kill(pid_, SIGCONT);
        auto end = std::chrono::steady_clock::now();
        int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();
        if (outMs_) *outMs_ = ms;
    }

private:
    bool isTidStopped(int tid) const {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid_, tid);
        int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return true; // thread exited
        char buf[256];
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0) return true;
        buf[n] = '\0';
        const char* rp = std::strrchr(buf, ')');
        if (!rp || rp[1] != ' ' || rp[2] == '\0') return true;
        char st = rp[2];
        return st == 'T' || st == 't' || st == 'Z' || st == 'X';
    }

    int pid_;
    bool active_;
    int64_t* outMs_;
    std::vector<int> tids_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace artseek
