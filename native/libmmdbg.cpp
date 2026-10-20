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
#include <elf.h>
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
// 优先写 /storage/emulated/0/MT2/log/（直接能在手机上看），
// 写不进去（targetSdk 30 的分区存储）就退回游戏私有目录。
static std::string g_logDir = "/data/data/com.hermes.h1game/files";
static bool g_logOnSdcard = false;
static FILE* g_fp = nullptr;
static pthread_mutex_t g_logMtx = PTHREAD_MUTEX_INITIALIZER;

static void pick_log_dir() {
    const char* cands[] = {
        "/storage/emulated/0/MT2/log",
        "/sdcard/MT2/log",
        "/data/data/com.hermes.h1game/files",
    };
    for (int i = 0; i < 3; i++) {
        std::string dir = cands[i];
        std::string probe = dir + "/.wtest";
        FILE* f = fopen(probe.c_str(), "w");
        if (f) {
            fclose(f);
            unlink(probe.c_str());
            g_logDir = dir;
            g_logOnSdcard = (i < 2);
            return;
        }
    }
}

static FILE* logfp() {
    if (g_fp) return g_fp;
    std::string p = g_logDir + "/mmdbg.log";
    g_fp = fopen(p.c_str(), "a");
    if (!g_fp && g_logDir != "/data/data/com.hermes.h1game/files") {
        g_logDir = "/data/data/com.hermes.h1game/files";
        g_logOnSdcard = false;
        p = g_logDir + "/mmdbg.log";
        g_fp = fopen(p.c_str(), "a");
    }
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

// dlopen(".../libil2cpp.so") 会失败：
//   library "libssgamesdkcronet.so" not found ... in namespace clns-13
// 我们的 so 在 classloader-namespace，解析不到游戏自己的那些内部依赖。
// 所以不走 dlopen —— 从 maps 拿基址，手工解析 ELF 的 .dynsym 表取符号。
static uintptr_t maps_base(const char* soname) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t addrs[128];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, soname)) continue;
        uintptr_t start = strtoull(line, nullptr, 16);
        if (n < 128) addrs[n++] = start;
    }
    fclose(f);
    uintptr_t lowest = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* p = (uint8_t*)addrs[i];
        if (p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') return addrs[i];
        if (!lowest || addrs[i] < lowest) lowest = addrs[i];
    }
    return lowest;
}

static uintptr_t elf_sym(uintptr_t base, const char* name) {
    if (!base) return 0;
    ElfW(Ehdr)* eh = (ElfW(Ehdr)*)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return 0;

    uintptr_t dyn = 0;
    ElfW(Phdr)* ph = (ElfW(Phdr)*)(base + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dyn = base + ph[i].p_vaddr; break; }
    }
    if (!dyn) return 0;

    uintptr_t symtab = 0, strtab = 0, hash = 0, gnu = 0;
    for (ElfW(Dyn)* d = (ElfW(Dyn)*)dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = d->d_un.d_ptr; break;
            case DT_STRTAB:   strtab = d->d_un.d_ptr; break;
            case DT_HASH:     hash   = d->d_un.d_ptr; break;
            case DT_GNU_HASH: gnu    = d->d_un.d_ptr; break;
            default: break;
        }
    }
    if (symtab < base) symtab += base;
    if (strtab < base) strtab += base;
    if (hash && hash < base) hash += base;
    if (gnu && gnu < base) gnu += base;
    if (!symtab || !strtab) return 0;

    uint32_t nsym = 0;
    if (hash) {
        nsym = ((uint32_t*)hash)[1];
    } else if (gnu) {
        uint32_t* g = (uint32_t*)gnu;
        uint32_t nbuckets = g[0], symoffset = g[1], bloom = g[2];
        uint32_t* buckets = &g[4 + bloom * 2];
        uint32_t* chain = &buckets[nbuckets];
        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
        if (last >= symoffset) {
            while (!(chain[last - symoffset] & 1)) last++;
            nsym = last + 1;
        }
    }
    if (!nsym) return 0;
    if (nsym > 200000) nsym = 200000;

    ElfW(Sym)* syms = (ElfW(Sym)*)symtab;
    const char* strs = (const char*)strtab;
    size_t nameLen = strlen(name);
    for (uint32_t i = 0; i < nsym; i++) {
        uint32_t off = syms[i].st_name;
        if (off == 0 || off > 0xFFFFFF) continue;
        const char* s = strs + off;
        size_t k = 0;
        while (k <= nameLen && s[k] == name[k]) k++;
        if (k == nameLen + 1) return base + syms[i].st_value;
    }
    return 0;
}

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
    uintptr_t base = maps_base("libil2cpp.so");
    if (!base) { LOGE("[IL2CPP] libil2cpp.so not found in /proc/self/maps"); return false; }
    LOGI("[IL2CPP] base=%p (elf symtab mode)", (void*)base);

    int missing = 0;
    #define B(field, sym) g_api.field = (fn_##field)elf_sym(base, sym); \
        if (!g_api.field) { missing++; LOGI("[IL2CPP] MISSING %s", sym); }
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

    LOGI("[IL2CPP] symbols resolved, missing=%d", missing);
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

// ============================= logcat 捕获 =============================
// 普通应用没有 READ_LOGS，这里的 popen 大概率读不到东西。
// 真抓到就写文件；抓不到就把原因记一笔，免得以后又猜。
static void* logcat_thread(void*) {
    std::string path = g_logDir + "/logcat.log";
    FILE* p = popen("logcat -v time 2>&1", "r");
    if (!p) {
        LOGI("[LOGCAT] popen failed, will rely on the root-side capture");
        return nullptr;
    }
    FILE* out = fopen(path.c_str(), "w");
    if (!out) {
        LOGI("[LOGCAT] cannot open %s", path.c_str());
        pclose(p);
        return nullptr;
    }
    LOGI("[LOGCAT] capturing -> %s", path.c_str());
    char buf[4096];
    long bytes = 0;
    int lines = 0;
    int denied = 0;
    while (fgets(buf, sizeof(buf), p)) {
        if (strstr(buf, "Permission denied") || strstr(buf, "not allowed")) denied++;
        fputs(buf, out);
        bytes += (long)strlen(buf);
        if (++lines % 50 == 0) fflush(out);
        if (bytes > 8L * 1024 * 1024) {
            fclose(out);
            std::string old = path + ".1";
            rename(path.c_str(), old.c_str());
            out = fopen(path.c_str(), "w");
            bytes = 0;
            if (!out) break;
        }
    }
    if (out) fclose(out);
    pclose(p);
    LOGI("[LOGCAT] thread exit, deniedLines=%d", denied);
    return nullptr;
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
    snprintf(buf, sizeof(buf), "bound=%d frame=%d stage=%d\nlog=%s%s",
             g_bound, g_frame, g_probeStage,
             g_logDir.c_str(), g_logOnSdcard ? " (sdcard)" : " (private)");
    return env->NewStringUTF(buf);
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    pick_log_dir();
    LOGI("===== libmmdbg.so loaded =====");
    LOGI("[LOG] dir=%s sdcard=%d", g_logDir.c_str(), (int)g_logOnSdcard);
    pthread_t t;
    if (pthread_create(&t, nullptr, logcat_thread, nullptr) == 0) pthread_detach(t);
    return JNI_VERSION_1_6;
}
