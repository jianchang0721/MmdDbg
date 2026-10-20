// =====================================================================
// libmmdbg.so —— MmdDbg 调试壳的动态逻辑
//
// 整个模块的玩法：
//   壳 APK 只负责把这玩意的副本 dlopen 进游戏进程（见 HookEntry.java）
//   改 C++ 之后：编译 -> 推成 libmmdbg_latest.so -> 点「重载 so」
//   不需要重装 APK，不需要重启游戏
//
// 主线程怎么拿到的：不搞 inline hook。
//   Java 侧用 Handler 每 16ms 调一次 nativeTick()，我们就在这个 tick 里干活。
//   这样完全避开 hook 框架，也不会在高频 hook 里做危险操作。
// =====================================================================
#include <jni.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <map>

#include "il2cpp_api.h"

#define TAG "MmdDbg"

// il2cpp_api.h 里声明的是 extern，这里给出定义
Il2CppApi g_api;

// ============================= 日志 =============================
static std::string g_logPath = "/data/data/com.hermes.h1game/files/mmdbg.log";
static FILE* g_fp = nullptr;
static pthread_mutex_t g_logMtx = PTHREAD_MUTEX_INITIALIZER;

static FILE* logfp() {
    if (g_fp) return g_fp;
    g_fp = fopen(g_logPath.c_str(), "a");
    return g_fp;
}

static void flog(const char* fmt, ...) {
    pthread_mutex_lock(&g_logMtx);
    FILE* f = logfp();
    if (f) {
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
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
    pthread_mutex_unlock(&g_logMtx);
}

#define LOGI(...) flog(__VA_ARGS__)
#define LOGE(...) flog(__VA_ARGS__)

// ============================= il2cpp 绑定 =============================
static bool g_bound = false;

// dlopen("libil2cpp.so") 在游戏进程里找不到 —— Android 的 linker namespace
// 不会按短名解析从 APK lib 目录加载的 so。得从 /proc/self/maps 把真实路径捞出来。
static std::string find_lib_path(const char* needle) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return "";
    char line[1024];
    std::string found;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, needle)) continue;
        char* p = strchr(line, '/');
        if (!p) continue;
        found = p;
        while (!found.empty() && (found.back() == '\n' || found.back() == '\r' ||
                                  found.back() == ' ')) found.pop_back();
        break;
    }
    fclose(f);
    return found;
}

static bool bind_api() {
    std::string path = find_lib_path("libil2cpp.so");
    void* h = nullptr;
    int how = 0;
    if (!path.empty()) {
        h = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (!h) { h = dlopen(path.c_str(), RTLD_NOW); how = 2; }
        LOGI("[IL2CPP] path=%s handle=%p mode=%d", path.c_str(), h, how);
    } else {
        h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (!h) { h = dlopen("libil2cpp.so", RTLD_NOW); how = 1; }
        LOGI("[IL2CPP] shortname handle=%p mode=%d", h, how);
    }
    if (!h) { LOGE("[IL2CPP] dlopen libil2cpp failed: %s", dlerror()); return false; }

    #define B(field, sym) g_api.field = (fn_##field)dlsym(h, sym); \
        if (!g_api.field) LOGE("[IL2CPP] MISSING %s", sym)
    B(domain_get,               "il2cpp_domain_get");
    B(domain_get_assemblies,    "il2cpp_domain_get_assemblies");
    B(thread_attach,            "il2cpp_thread_attach");
    B(assembly_get_image,       "il2cpp_assembly_get_image");
    B(image_get_name,           "il2cpp_image_get_name");
    B(image_get_class_count,    "il2cpp_image_get_class_count");
    B(image_get_class,          "il2cpp_image_get_class");
    B(class_from_name,          "il2cpp_class_from_name");
    B(class_get_name,           "il2cpp_class_get_name");
    B(class_get_namespace,      "il2cpp_class_get_namespace");
    B(class_get_parent,         "il2cpp_class_get_parent");
    B(class_is_assignable_from, "il2cpp_class_is_assignable_from");
    B(class_get_type,           "il2cpp_class_get_type");
    B(class_get_methods,        "il2cpp_class_get_methods");
    B(method_get_name,          "il2cpp_method_get_name");
    B(method_get_param_count,   "il2cpp_method_get_param_count");
    B(method_get_param,         "il2cpp_method_get_param");
    B(class_from_type,          "il2cpp_class_from_type");
    B(runtime_invoke,           "il2cpp_runtime_invoke");
    B(string_new,               "il2cpp_string_new");
    B(object_new,               "il2cpp_object_new");
    B(object_get_class,         "il2cpp_object_get_class");
    B(array_new,                "il2cpp_array_new");
    B(type_get_object,          "il2cpp_type_get_object");
    B(class_get_fields,         "il2cpp_class_get_fields");
    B(field_get_name,           "il2cpp_field_get_name");
    B(field_get_offset,         "il2cpp_field_get_offset");
    B(field_get_type,           "il2cpp_field_get_type");
    B(type_get_name,            "il2cpp_type_get_name");
    B(class_array_element_size, "il2cpp_class_array_element_size");
    #undef B

    if (!g_api.domain_get || !g_api.class_from_name || !g_api.runtime_invoke) {
        LOGE("[IL2CPP] core api missing, bind failed");
        return false;
    }
    // 把自己 attach 到 il2cpp 线程（tick 本来就在主线程，保险）
    if (g_api.thread_attach) {
        Il2CppDomain* d = g_api.domain_get();
        if (d) g_api.thread_attach(d);
    }
    g_bound = true;
    LOGI("[IL2CPP] bind ok");
    return true;
}

// ============================= 基础工具 =============================
static std::string to_utf8(Il2CppString* s) {
    if (!s) return "";
    // Il2CppString: klass, monitor, length(int32), chars[utf16]
    char* p = (char*)s;
    int32_t len = *(int32_t*)(p + 16);
    const uint16_t* ch = (const uint16_t*)(p + 20);
    std::string out;
    out.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        uint16_t c = ch[i];
        if (c < 0x80) out.push_back((char)c);
        else if (c < 0x800) {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (c >> 12)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

static std::string obj_name(Il2CppObject* o) {
    if (!o) return "";
    Il2CppClass* k = g_api.object_get_class(o);
    if (!k) return "";
    return std::string(g_api.class_get_name(k) ? g_api.class_get_name(k) : "?");
}

static Il2CppClass* find_class_any(const char* ns, const char* name) {
    if (!g_api.domain_get) return nullptr;
    Il2CppDomain* d = g_api.domain_get();
    size_t n = 0;
    const Il2CppAssembly** as = g_api.domain_get_assemblies(d, &n);
    for (size_t i = 0; i < n; i++) {
        Il2CppImage* img = g_api.assembly_get_image(as[i]);
        if (!img) continue;
        Il2CppClass* k = g_api.class_from_name(img, ns, name);
        if (k) return k;
    }
    return nullptr;
}

static const MethodInfo* find_method_any(Il2CppClass* klass, const char* name) {
    if (!klass) return nullptr;
    for (Il2CppClass* k = klass; k; k = g_api.class_get_parent(k)) {
        void* iter = nullptr;
        const MethodInfo* m;
        while ((m = g_api.class_get_methods(k, &iter))) {
            const char* nm = g_api.method_get_name(m);
            if (nm && strcmp(nm, name) == 0) return m;
        }
    }
    return nullptr;
}

static Il2CppObject* invoke_inst(const MethodInfo* m, Il2CppObject* self, void** args) {
    if (!m) return nullptr;
    Il2CppException* exc = nullptr;
    Il2CppObject* r = g_api.runtime_invoke(m, self, args, &exc);
    if (exc) { LOGE("[IL2CPP] invoke %s threw", g_api.method_get_name(m)); return nullptr; }
    return r;
}

static Il2CppObject* invoke_static(const MethodInfo* m, void** args) {
    return invoke_inst(m, nullptr, args);
}

static int32_t unbox_i32(Il2CppObject* boxed) {
    if (!boxed) return 0;
    return *(int32_t*)((char*)boxed + 16);
}

static int array_elem_size(Il2CppClass* k, int fallback) {
    if (k && g_api.class_array_element_size) {
        int32_t n = g_api.class_array_element_size(k);
        if (n > 0) return n;
    }
    return fallback;
}

// ============================= SMR 方法全量 dump =============================
static void dump_methods(Il2CppClass* klass, const char* tag) {
    if (!klass) { LOGE("[IL2CPP] dump %s: class null", tag); return; }
    int total = 0;
    for (Il2CppClass* k = klass; k; k = g_api.class_get_parent(k)) {
        const char* cn = g_api.class_get_name(k);
        LOGI("[IL2CPP] --- %s : %s ---", tag, cn ? cn : "?");
        void* iter = nullptr;
        const MethodInfo* m;
        int n = 0;
        while ((m = g_api.class_get_methods(k, &iter))) {
            const char* nm = g_api.method_get_name(m);
            LOGI("[IL2CPP]   %s/%u", nm ? nm : "?", g_api.method_get_param_count(m));
            if (++n > 200) break;
        }
        total += n;
    }
    LOGI("[IL2CPP] %s: %d methods dumped", tag, total);
}

// ============================= 探针 =============================
static int g_frame = 0;
static int g_probeStage = 0;

static void probe_step() {
    // 分步骤跑，每步隔几帧，方便定位崩在哪
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) { LOGE("[SKIN] SkinnedMeshRenderer class not found yet"); return; }

    if (g_probeStage == 0) {
        g_probeStage = 1;
        LOGI("========== [SKIN] PROBE BEGIN ==========");
        LOGI("[IL2CPP] ---- SkinnedMeshRenderer all methods ----");
        dump_methods(SMRCls, "SMR");
        const char* keys[] = {"set_sharedMesh","get_sharedMesh","set_bones","get_bones",
                              "set_rootBone","get_rootBone","set_bounds","get_bounds",
                              "set_localBounds","BakeMesh","set_updateWhenOffscreen"};
        for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
            const MethodInfo* m = find_method_any(SMRCls, keys[i]);
            LOGI("[IL2CPP] resolve %-24s -> %p argc=%u", keys[i], (void*)m,
                 m ? g_api.method_get_param_count(m) : 0);
        }
        LOGI("========== [SKIN] PROBE END ==========");
    }
}

// ============================= tick 入口 =============================
extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdbg_HookEntry_nativeTick(JNIEnv*, jclass) {
    if (!g_bound) {
        static int tries = 0;
        if ((tries++ % 60) != 0) return;      // 别每帧刷屏
        if (!bind_api()) return;
        LOGI("[SKIN] api bound, waiting for scene...");
    }
    g_frame++;
    if (g_frame == 60) {
        LOGI("[SKIN] frame %d, start probe", g_frame);
        probe_step();
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_moss_mmdbg_HookEntry_nativeStatus(JNIEnv* env, jclass) {
    char buf[512];
    snprintf(buf, sizeof(buf), "bound=%d frame=%d stage=%d log=%s",
             g_bound, g_frame, g_probeStage, g_logPath.c_str());
    return env->NewStringUTF(buf);
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    LOGI("===== libmmdbg.so loaded =====");
    return JNI_VERSION_1_6;
}
