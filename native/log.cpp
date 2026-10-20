// =====================================================================
// libmmdbg_log.so —— 独立的日志模块
//
// 跟模型模块完全解耦：
//   - 不碰 Unity、不碰 il2cpp，不需要主线程 tick
//   - JNI_OnLoad 里自己起线程，写自己的日志、尝试抓 logcat
//   - 改了它只需要替换 libmmdbg_log.so，模型那边一行都不用动
//
// 注意：游戏进程没有 READ_LOGS 权限，这里 popen("logcat") 大概率读不到东西。
// 抓不到就把原因写清楚，别让以后的人再猜。
// =====================================================================
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <android/log.h>
#include <string>

#define TAG "MmdDbgLog"

static std::string g_dir = "/data/data/com.hermes.h1game/files";
static std::string g_logPath;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static FILE* g_fp = nullptr;

static void pick_dir() {
    const char* cands[] = {
        "/storage/emulated/0/MT2/log",
        "/sdcard/MT2/log",
        "/data/data/com.hermes.h1game/files",
    };
    for (int i = 0; i < 3; i++) {
        std::string probe = std::string(cands[i]) + "/.lwtest";
        FILE* f = fopen(probe.c_str(), "w");
        if (f) { fclose(f); unlink(probe.c_str()); g_dir = cands[i]; break; }
    }
    g_logPath = g_dir + "/mmdbg_log.log";
}

static FILE* logfp() {
    if (g_fp) return g_fp;
    g_fp = fopen(g_logPath.c_str(), "a");
    if (!g_fp && g_dir != "/data/data/com.hermes.h1game/files") {
        g_dir = "/data/data/com.hermes.h1game/files";
        g_logPath = g_dir + "/mmdbg_log.log";
        g_fp = fopen(g_logPath.c_str(), "a");
    }
    return g_fp;
}

static void L(const char* fmt, ...) {
    pthread_mutex_lock(&g_mtx);
    FILE* f = logfp();
    if (f) {
        time_t t = time(nullptr);
        struct tm tmv; localtime_r(&t, &tmv);
        fprintf(f, "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fflush(f);
    }
    va_list ap2; va_start(ap2, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, TAG, fmt, ap2);
    va_end(ap2);
    pthread_mutex_unlock(&g_mtx);
}

// 日志超过 4MB 就重新开一个，别把手机塞满
static void rotate_if_big() {
    struct stat st;
    if (stat(g_logPath.c_str(), &st) == 0 && st.st_size > 4L * 1024 * 1024) {
        if (g_fp) { fclose(g_fp); g_fp = nullptr; }
        std::string old = g_logPath + ".1";
        rename(g_logPath.c_str(), old.c_str());
    }
}

// 试一次 logcat，把结果写清楚
static void probe_logcat() {
    L("--- logcat probe ---");
    FILE* p = popen("logcat -d -t 20 2>&1", "r");
    if (!p) { L("[LOGCAT] popen failed"); return; }
    char buf[512];
    int lines = 0, denied = 0;
    std::string first;
    while (fgets(buf, sizeof(buf), p) && lines < 40) {
        lines++;
        if (first.empty()) first = buf;
        if (strstr(buf, "Permission denied") || strstr(buf, "not allowed") ||
            strstr(buf, "couldn't open")) denied++;
    }
    pclose(p);
    L("[LOGCAT] got %d lines, denied/err %d", lines, denied);
    if (!first.empty()) {
        std::string s = first;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        L("[LOGCAT] first: %s", s.c_str());
    }
    if (denied > 0 || lines == 0) {
        L("[LOGCAT] 这个进程没有 READ_LOGS 权限，logcat 抓不到。");
        L("[LOGCAT] 全量日志靠 root 侧脚本: bash ~/mmdbg-log.sh start");
    } else {
        L("[LOGCAT] 居然能读到，在线抓取已关闭（全量交给 root 侧脚本）");
    }
}

static void* worker(void*) {
    pick_dir();
    rotate_if_big();
    L("===== libmmdbg_log.so loaded =====");
    L("[LOG] dir=%s", g_dir.c_str());
    probe_logcat();

    // 心跳：证明这个 so 还活着
    int beat = 0;
    while (true) {
        sleep(30);
        beat++;
        rotate_if_big();
        L("[BEAT] %d  uptime=%ds", beat, beat * 30);
    }
    return nullptr;
}

JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
    return JNI_VERSION_1_6;
}
