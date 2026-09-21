#include "bridge/go_probe.h"

#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "GoProbe"

#define GP_LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)
#define GP_LOGE(fmt, ...) OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

namespace gobridge {

namespace {

typedef char* (*GoHelloFn)();
typedef void (*GoFreeFn)(char* p);
typedef int (*GoStartTickerFn)();
typedef long long (*GoTicksFn)();

// S5: 网络与 TUN fd
typedef char* (*GoUDPStartFn)();
typedef char* (*GoUDPStatsFn)();
typedef char* (*GoUDPSendToFn)(char* hostPort, char* payload);
typedef char* (*GoDNSLookupFn)(char* host);
typedef char* (*GoTunStartFn)(int fd, char* name);
typedef int (*GoTunWriteFn)(int fd, char* hexPkt);
typedef char* (*GoTunStatsFn)();
typedef char* (*GoTunCaptureFn)(int fd, char* dst, char* marker);
typedef char* (*GoTunBuildUDPFn)(char* srcIp, char* dstIp, int sport, int dport, char* payload);

void* g_handle = nullptr;
GoHelloFn g_hello = nullptr;
GoFreeFn g_free = nullptr;
GoStartTickerFn g_startTicker = nullptr;
GoTicksFn g_ticks = nullptr;

// S5 符号: 允许缺失(旧 .so 仍可用基础探针), 由各入口自行判空
GoUDPStartFn g_udpStart = nullptr;
GoUDPStatsFn g_udpStats = nullptr;
GoUDPSendToFn g_udpSendTo = nullptr;
GoDNSLookupFn g_dnsLookup = nullptr;
GoTunStartFn g_tunStart = nullptr;
GoTunWriteFn g_tunWrite = nullptr;
GoTunStatsFn g_tunStats = nullptr;
GoTunCaptureFn g_tunCapture = nullptr;
GoTunBuildUDPFn g_tunBuildUDP = nullptr;

// 诊断用: Go 运行时的 fatal/panic 输出走 fd 2, 重定向到应用沙箱文件,
// 崩溃后可在下次启动时读回并打到 hilog.
// UI 进程和 VPN 扩展进程会各自加载一次核心, 所以文件名带 pid, 避免互相清空.
const char* kDbgLogDir = "/data/storage/el2/base/files";

static const std::string& DebugLogPath()
{
    static const std::string path =
        std::string(kDbgLogDir) + "/go_dbg_" + std::to_string(::getpid()) + ".log";
    return path;
}

static void RedirectStderrToDebugLog()
{
    static bool redirected = false;
    if (redirected) {
        return;
    }
    const std::string& path = DebugLogPath();
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        GP_LOGE("stderr 重定向失败(%{public}s): %{public}s", path.c_str(), std::strerror(errno));
        return;
    }
    ::dup2(fd, 2);
    if (fd != 2) {
        ::close(fd);
    }
    redirected = true;
    GP_LOGI("Go 核心日志文件: %{public}s", path.c_str());
}

// 找出非本进程的最新一份 go_dbg_*.log(即上一次运行留下的)回显到 hilog
static void DumpDebugLog()
{
    std::string best;
    time_t bestTime = 0;
    DIR* dir = ::opendir(kDbgLogDir);
    if (dir == nullptr) {
        return;
    }
    const std::string self = DebugLogPath();
    while (struct dirent* ent = ::readdir(dir)) {
        std::string name = ent->d_name;
        if (name.rfind("go_dbg_", 0) != 0 || name.rfind(".log") != name.size() - 4) {
            continue;
        }
        std::string full = std::string(kDbgLogDir) + "/" + name;
        if (full == self) {
            continue;
        }
        struct stat st = {};
        if (::stat(full.c_str(), &st) != 0) {
            continue;
        }
        if (st.st_mtime >= bestTime) {
            bestTime = st.st_mtime;
            best = full;
        }
    }
    ::closedir(dir);
    if (best.empty()) {
        return;
    }

    FILE* f = std::fopen(best.c_str(), "rb");
    if (f == nullptr) {
        return;
    }
    const long long kTailBytes = 192 * 1024;
    long long total = 0;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        total = static_cast<long long>(std::ftell(f));
    }
    long long start = (total > kTailBytes) ? (total - kTailBytes) : 0;
    if (std::fseek(f, static_cast<long>(start), SEEK_SET) != 0) {
        std::fclose(f);
        return;
    }
    std::string all;
    char buf[512];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        all.append(buf, n);
    }
    std::fclose(f);
    if (start > 0) {
        size_t firstNl = all.find('\n');
        if (firstNl == std::string::npos) {
            return;
        }
        all.erase(0, firstNl + 1);
    }
    if (all.empty()) {
        return;
    }
    GP_LOGI("上次运行日志尾部 %{public}s (共 %lld 字节, 回放尾部 %zu 字节):", best.c_str(), total, all.size());

    // 只回放最后 110 行 (崩溃现场在文件末尾)
    const size_t kKeepLines = 110;
    std::string keep[kKeepLines];
    size_t kept = 0;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        if (nl == std::string::npos) {
            nl = all.size();
        }
        std::string line = all.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) {
            continue;
        }
        if (line.size() > 320) {
            line.resize(320);
        }
        keep[kept % kKeepLines] = std::move(line);
        kept++;
    }
    size_t emit = kept < kKeepLines ? kept : kKeepLines;
    for (size_t i = 0; i < emit; i++) {
        GP_LOGI("[dbg] %{public}s", keep[(kept - emit + i) % kKeepLines].c_str());
    }
}

// 返回值: 空字符串表示成功, 否则为错误描述
std::string EnsureLoaded()
{
    if (g_handle != nullptr) {
        return "";
    }
    DumpDebugLog();
    RedirectStderrToDebugLog();
    GP_LOGI("dlopen 开始: libgohello.so");
    dlerror();
    g_handle = dlopen("libgohello.so", RTLD_NOW | RTLD_GLOBAL);
    GP_LOGI("dlopen 返回: handle=%{public}s", (g_handle != nullptr ? "ok" : "null"));
    if (g_handle == nullptr) {
        const char* err = dlerror();
        return std::string("dlopen 失败: ") + (err != nullptr ? err : "unknown");
    }
    g_hello = reinterpret_cast<GoHelloFn>(dlsym(g_handle, "GoHello"));
    g_free = reinterpret_cast<GoFreeFn>(dlsym(g_handle, "GoFree"));
    g_startTicker = reinterpret_cast<GoStartTickerFn>(dlsym(g_handle, "GoStartTicker"));
    g_ticks = reinterpret_cast<GoTicksFn>(dlsym(g_handle, "GoTicks"));

    g_udpStart = reinterpret_cast<GoUDPStartFn>(dlsym(g_handle, "GoUDPStart"));
    g_udpStats = reinterpret_cast<GoUDPStatsFn>(dlsym(g_handle, "GoUDPStats"));
    g_udpSendTo = reinterpret_cast<GoUDPSendToFn>(dlsym(g_handle, "GoUDPSendTo"));
    g_dnsLookup = reinterpret_cast<GoDNSLookupFn>(dlsym(g_handle, "GoDNSLookup"));
    g_tunStart = reinterpret_cast<GoTunStartFn>(dlsym(g_handle, "GoTunStart"));
    g_tunWrite = reinterpret_cast<GoTunWriteFn>(dlsym(g_handle, "GoTunWrite"));
    g_tunStats = reinterpret_cast<GoTunStatsFn>(dlsym(g_handle, "GoTunStats"));
    g_tunCapture = reinterpret_cast<GoTunCaptureFn>(dlsym(g_handle, "GoTunCapture"));
    g_tunBuildUDP = reinterpret_cast<GoTunBuildUDPFn>(dlsym(g_handle, "GoTunBuildUDP"));
    if (g_hello == nullptr || g_free == nullptr || g_startTicker == nullptr || g_ticks == nullptr) {
        return "符号解析失败: 导出的 Go 函数不完整";
    }
    return "";
}

napi_value MakeString(napi_env env, const std::string& s)
{
    napi_value result = nullptr;
    napi_create_string_utf8(env, s.c_str(), NAPI_AUTO_LENGTH, &result);
    return result;
}

} // namespace

// ==== TLS 布局探针 (spike): 验证 OHOS musl 下能否为 Go runtime.tls_g 求出 TP 相对偏移 ====

static inline uintptr_t ReadTp()
{
    uintptr_t tp = 0;
#if defined(__aarch64__)
    __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(tp));
#endif
    return tp;
}

// 显式 global-dynamic: 走 TLSGD/TLSDESC, 不产生 IE 模型重定位
__attribute__((tls_model("global-dynamic"))) static thread_local uintptr_t gTlsProbeSlot;

static long TlsVarOffset()
{
    return static_cast<long>(reinterpret_cast<char*>(&gTlsProbeSlot) - reinterpret_cast<char*>(ReadTp()));
}

// 只在 TP 所在的内存映射范围内扫描, 避免越界读
static long ScanTlsForMagic(uintptr_t tp, uintptr_t magic, long maxWords)
{
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) {
        return -2;
    }
    char line[512];
    unsigned long lo = 0;
    unsigned long hi = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long a = 0;
        unsigned long b = 0;
        if (std::sscanf(line, "%lx-%lx", &a, &b) == 2 && tp >= a && tp < b) {
            lo = a;
            hi = b;
            break;
        }
    }
    std::fclose(f);
    if (hi == 0) {
        return -3;
    }
    long maxIdx = static_cast<long>((hi - tp) / sizeof(uintptr_t));
    if (maxIdx > maxWords) {
        maxIdx = maxWords;
    }
    for (long i = 0; i < maxIdx; i++) {
        if (*reinterpret_cast<uintptr_t*>(tp + static_cast<uintptr_t>(i) * sizeof(uintptr_t)) == magic) {
            return i;
        }
    }
    return -1;
}

struct MapRange {
    uintptr_t lo;
    uintptr_t hi;
    bool rw;
};

static int ReadMaps(MapRange* out, int cap)
{
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) {
        return 0;
    }
    char line[512];
    int n = 0;
    while (n < cap && std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long a = 0;
        unsigned long b = 0;
        char perms[8] = {0};
        if (std::sscanf(line, "%lx-%lx %7s", &a, &b, perms) >= 3) {
            out[n].lo = a;
            out[n].hi = b;
            out[n].rw = (perms[0] == 'r' && perms[1] == 'w');
            n++;
        }
    }
    std::fclose(f);
    return n;
}

static bool InMaps(const MapRange* m, int n, uintptr_t p, size_t need, bool requireRw)
{
    for (int i = 0; i < n; i++) {
        if (p >= m[i].lo && p < m[i].hi && (!requireRw || m[i].rw) && (p + need) <= m[i].hi) {
            return true;
        }
    }
    return false;
}

static pthread_key_t gProbeKey;
static uintptr_t gProbeMagic = 0x23581321345589ULL;

// 在 TP 附近扫描 struct pthread, 找出指向 TSD 数组的那个指针字段,
// 返回字段地址相对 TP 的有符号字节偏移 (负值表示字段在 TP 之下)
static long FindTsdFieldOffset(uintptr_t tp, pthread_key_t k)
{
    MapRange maps[256];
    int n = ReadMaps(maps, 256);
    if (n == 0) {
        return -10;
    }
    const long words = 256; // +/- 2KB
    for (long i = -words; i <= words; i++) {
        uintptr_t w = static_cast<uintptr_t>(static_cast<long>(tp) + i * 8);
        if (!InMaps(maps, n, w, 8, false)) {
            continue;
        }
        uintptr_t v = *reinterpret_cast<uintptr_t*>(w);
        if (!InMaps(maps, n, v, static_cast<size_t>(k) * 8 + 8, true)) {
            continue;
        }
        if (*reinterpret_cast<uintptr_t*>(v + static_cast<size_t>(k) * 8) == gProbeMagic) {
            return i * 8;
        }
    }
    return -12;
}

struct ThrResult {
    long selfOff = -1;   // pthread_self() - TP
    long fieldOff = -1;  // 本线程自己扫出来的 TSD 字段偏移
    int slotOk = 0;      // 用主线程的 fieldOff 能否在本线程读到 magic
};

static void ProbeOnThread(ThrResult* out, long fieldOffMain)
{
    uintptr_t tp = ReadTp();
    pthread_setspecific(gProbeKey, reinterpret_cast<void*>(gProbeMagic));
    out->selfOff = static_cast<long>(reinterpret_cast<uintptr_t>(pthread_self()) - tp);
    out->fieldOff = FindTsdFieldOffset(tp, gProbeKey);

    MapRange maps[256];
    int n = ReadMaps(maps, 256);
    uintptr_t f = static_cast<uintptr_t>(static_cast<long>(tp) + fieldOffMain);
    if (!InMaps(maps, n, f, 8, false)) {
        return;
    }
    uintptr_t arr = *reinterpret_cast<uintptr_t*>(f);
    size_t slot = arr + static_cast<size_t>(gProbeKey) * 8;
    if (arr != 0 && InMaps(maps, n, slot, 8, false) &&
        *reinterpret_cast<uintptr_t*>(slot) == gProbeMagic) {
        out->slotOk = 1;
    }
}

napi_value TlsProbe(napi_env env, napi_callback_info info)
{
    uintptr_t tp = ReadTp();
    long inlineScan = -4;
    long fieldOffMain = -20;
    ThrResult t1;
    ThrResult t2;
    long varMain = 0;
    long varThread = 0;
    char buf[400];

    if (pthread_key_create(&gProbeKey, nullptr) != 0) {
        return MakeString(env, "pthread_key_create failed");
    }
    pthread_setspecific(gProbeKey, reinterpret_cast<void*>(gProbeMagic));
    inlineScan = ScanTlsForMagic(tp, gProbeMagic, 384);
    fieldOffMain = FindTsdFieldOffset(tp, gProbeKey);

    std::thread ta([&t1, fieldOffMain]() { ProbeOnThread(&t1, fieldOffMain); });
    ta.join();
    std::thread tb([&t2, fieldOffMain]() { ProbeOnThread(&t2, fieldOffMain); });
    tb.join();

    pthread_setspecific(gProbeKey, nullptr);
    pthread_key_delete(gProbeKey);

    varMain = TlsVarOffset();
    std::thread tc([&varThread]() { varThread = TlsVarOffset(); });
    tc.join();

    std::snprintf(buf, sizeof(buf),
        "tp=0x%llx inlineScan=%ld fieldOffMain=%ld | T1 selfOff=%ld fieldOff=%ld slotOk=%d | "
        "T2 selfOff=%ld fieldOff=%ld slotOk=%d | tlsVarMain=%ld tlsVarThread=%ld",
        static_cast<unsigned long long>(tp), inlineScan, fieldOffMain,
        t1.selfOff, t1.fieldOff, t1.slotOk,
        t2.selfOff, t2.fieldOff, t2.slotOk,
        varMain, varThread);
    std::string report(buf);
    GP_LOGI("TLS probe: %{public}s", report.c_str());
    return MakeString(env, report);
}

napi_value Probe(napi_env env, napi_callback_info info)
{
    std::string err = EnsureLoaded();
    if (!err.empty()) {
        GP_LOGE("Go runtime probe failed: %{public}s", err.c_str());
        return MakeString(env, "加载失败 | " + err);
    }
    char* s = g_hello();
    std::string payload = (s != nullptr) ? s : "(GoHello 返回空)";
    if (s != nullptr) {
        g_free(s);
    }
    std::string report = "加载成功 | " + payload;
    GP_LOGI("Go runtime probe ok: %{public}s", report.c_str());
    return MakeString(env, report);
}

napi_value StartTicker(napi_env env, napi_callback_info info)
{
    std::string err = EnsureLoaded();
    int rc = -1;
    if (err.empty()) {
        rc = g_startTicker();
        GP_LOGI("Go ticker started, rc=%{public}d", rc);
    } else {
        GP_LOGE("Go ticker start failed: %{public}s", err.c_str());
    }
    napi_value result = nullptr;
    napi_create_int32(env, rc, &result);
    return result;
}

napi_value Ticks(napi_env env, napi_callback_info info)
{
    long long t = -1;
    if (g_handle != nullptr && g_ticks != nullptr) {
        t = g_ticks();
    }
    napi_value result = nullptr;
    napi_create_int64(env, t, &result);
    return result;
}

// ==== S5: Go 侧网络能力 (UDP / DNS / TUN fd) ====

std::string TakeGoString(char* s)
{
    if (s == nullptr) {
        return "(Go 返回空指针)";
    }
    std::string out(s);
    g_free(s);
    return out;
}

std::string ArgString(napi_env env, napi_value v)
{
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) {
        return "";
    }
    std::string s(len, '\0');
    napi_get_value_string_utf8(env, v, s.data(), len + 1, &len);
    return s;
}

int32_t ArgInt(napi_env env, napi_value v)
{
    int32_t i = 0;
    napi_get_value_int32(env, v, &i);
    return i;
}

// 统一入口: 加载 .so + 检查符号, 返回空表示可用
// 传符号地址而不是符号值: dlsym 在 EnsureLoaded 里才赋值, 传值会读到加载前的 null
template <typename T>
std::string PrepareS5(const char* name, T* sym)
{
    std::string err = EnsureLoaded();
    if (!err.empty()) {
        return err;
    }
    if (*sym == nullptr) {
        return std::string("该 .so 未导出 ") + name + "(需重新编译 libgohello.so)";
    }
    return "";
}

napi_value UdpStart(napi_env env, napi_callback_info info)
{
    std::string err = PrepareS5("GoUDPStart", &g_udpStart);
    if (!err.empty()) {
        GP_LOGE("UDP start 失败: %{public}s", err.c_str());
        return MakeString(env, "ERR " + err);
    }
    std::string report = TakeGoString(g_udpStart());
    GP_LOGI("UDP start: %{public}s", report.c_str());
    return MakeString(env, report);
}

napi_value UdpStats(napi_env env, napi_callback_info info)
{
    std::string err = PrepareS5("GoUDPStats", &g_udpStats);
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    return MakeString(env, TakeGoString(g_udpStats()));
}

// args: hostPort, payload
napi_value UdpSendTo(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = PrepareS5("GoUDPSendTo", &g_udpSendTo);
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (argc < 2) {
        return MakeString(env, "ERR 需要 (hostPort, payload)");
    }
    std::string hp = ArgString(env, argv[0]);
    std::string payload = ArgString(env, argv[1]);
    std::string report = TakeGoString(g_udpSendTo(const_cast<char*>(hp.c_str()),
                                                  const_cast<char*>(payload.c_str())));
    GP_LOGI("UDP sendTo %{public}s: %{public}s", hp.c_str(), report.c_str());
    return MakeString(env, report);
}

napi_value DnsLookup(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = PrepareS5("GoDNSLookup", &g_dnsLookup);
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (argc < 1) {
        return MakeString(env, "ERR 需要 (host)");
    }
    std::string host = ArgString(env, argv[0]);
    std::string report = TakeGoString(g_dnsLookup(const_cast<char*>(host.c_str())));
    GP_LOGI("DNS lookup %{public}s: %{public}s", host.c_str(), report.c_str());
    return MakeString(env, report);
}

// args: fd
napi_value TunStart(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = PrepareS5("GoTunStart", &g_tunStart);
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (argc < 1) {
        return MakeString(env, "ERR 需要 (fd)");
    }
    int32_t fd = ArgInt(env, argv[0]);
    std::string report = TakeGoString(g_tunStart(fd, const_cast<char*>("tun")));
    GP_LOGI("TUN start fd=%{public}d: %{public}s", fd, report.c_str());
    return MakeString(env, report);
}

// args: fd, hexPkt
napi_value TunWrite(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = PrepareS5("GoTunWrite", &g_tunWrite);
    int32_t rc = -100;
    if (err.empty() && argc >= 2) {
        int32_t fd = ArgInt(env, argv[0]);
        std::string hexPkt = ArgString(env, argv[1]);
        rc = g_tunWrite(fd, const_cast<char*>(hexPkt.c_str()));
        GP_LOGI("TUN write fd=%{public}d 返回 %{public}d", fd, rc);
    }
    napi_value result = nullptr;
    napi_create_int32(env, rc, &result);
    return result;
}

napi_value TunStats(napi_env env, napi_callback_info info)
{
    std::string err = PrepareS5("GoTunStats", &g_tunStats);
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    return MakeString(env, TakeGoString(g_tunStats()));
}

// args: fd, dst(host:port), marker
napi_value TunCapture(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = PrepareS5("GoTunCapture", &g_tunCapture);
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (argc < 3) {
        return MakeString(env, "ERR 需要 (fd, dst, marker)");
    }
    int32_t fd = ArgInt(env, argv[0]);
    std::string dst = ArgString(env, argv[1]);
    std::string marker = ArgString(env, argv[2]);
    std::string report = TakeGoString(g_tunCapture(fd, const_cast<char*>(dst.c_str()),
                                                   const_cast<char*>(marker.c_str())));
    GP_LOGI("TUN capture fd=%{public}d dst=%{public}s: %{public}s", fd, dst.c_str(), report.c_str());
    return MakeString(env, report);
}

// args: srcIp, dstIp, sport, dport, payload -> 返回包的 hex
napi_value TunBuildUdp(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = PrepareS5("GoTunBuildUDP", &g_tunBuildUDP);
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (argc < 5) {
        return MakeString(env, "ERR 需要 (srcIp, dstIp, sport, dport, payload)");
    }
    std::string src = ArgString(env, argv[0]);
    std::string dst = ArgString(env, argv[1]);
    int32_t sport = ArgInt(env, argv[2]);
    int32_t dport = ArgInt(env, argv[3]);
    std::string payload = ArgString(env, argv[4]);
    return MakeString(env, TakeGoString(g_tunBuildUDP(const_cast<char*>(src.c_str()),
                                                      const_cast<char*>(dst.c_str()),
                                                      sport, dport,
                                                      const_cast<char*>(payload.c_str()))));
}

// ==================== Route B: ArkMesh Go 核心 ====================
// 独立句柄, 与 libgohello.so 的解包/加载路径完全隔离
typedef char* (*CoreSelfTestFn)();
typedef char* (*CoreNetCheckFn)(char* host);
typedef char* (*CoreIfProbeFn)();
typedef char* (*CoreControlUpFn)(char* controlURL, char* authKey, char* hostname, char* stateDir, char* exitNode);
typedef char* (*CoreControlStatusFn)();
typedef char* (*CorePingPeersFn)(char* type);
typedef char* (*CoreDnsProbeFn)(char* name, int timeoutMs);
typedef void (*CoreControlDownFn)();
typedef char* (*CoreAttachTunFn)(int fd, int mtu);
typedef void (*CoreFreeFn)(char* p);

void* g_coreHandle = nullptr;
CoreSelfTestFn g_coreSelfTest = nullptr;
CoreNetCheckFn g_coreNetCheck = nullptr;
CoreIfProbeFn g_coreIfProbe = nullptr;
CoreControlUpFn g_coreControlUp = nullptr;
CoreControlStatusFn g_coreControlStatus = nullptr;
CorePingPeersFn g_corePingPeers = nullptr;
CoreDnsProbeFn g_coreDnsProbe = nullptr;
CoreControlDownFn g_coreControlDown = nullptr;
CoreAttachTunFn g_coreAttachTun = nullptr;
CoreFreeFn g_coreFree = nullptr;

std::string EnsureCoreLoaded()
{
    if (g_coreHandle != nullptr) {
        return "";
    }
    DumpDebugLog();
    RedirectStderrToDebugLog();
    // 不要试图在这里用 setenv 给 Go 传 GODEBUG/GOMAXPROCS: OHOS fork 的 goenvs()
    // 读的是 /proc/self/environ(进程启动时的内核快照), 看不到进程启动后的 setenv。
    // 需要的话在 Go 侧用 os.Setenv 或 runtime.GOMAXPROCS() 设置。
    GP_LOGI("dlopen 开始: libarkmesh_core.so");
    dlerror();
    g_coreHandle = dlopen("libarkmesh_core.so", RTLD_NOW | RTLD_GLOBAL);
    GP_LOGI("dlopen(libarkmesh_core.so) 返回: handle=%{public}s",
            (g_coreHandle != nullptr ? "ok" : "null"));
    if (g_coreHandle == nullptr) {
        const char* err = dlerror();
        return std::string("dlopen 失败: ") + (err != nullptr ? err : "unknown");
    }
    g_coreSelfTest = reinterpret_cast<CoreSelfTestFn>(dlsym(g_coreHandle, "ArkMeshSelfTest"));
    g_coreNetCheck = reinterpret_cast<CoreNetCheckFn>(dlsym(g_coreHandle, "ArkMeshNetCheck"));
    g_coreIfProbe = reinterpret_cast<CoreIfProbeFn>(dlsym(g_coreHandle, "ArkMeshIfProbe"));
    g_coreControlUp = reinterpret_cast<CoreControlUpFn>(dlsym(g_coreHandle, "ArkMeshControlUp"));
    g_coreControlStatus = reinterpret_cast<CoreControlStatusFn>(dlsym(g_coreHandle, "ArkMeshControlStatus"));
    g_corePingPeers = reinterpret_cast<CorePingPeersFn>(dlsym(g_coreHandle, "ArkMeshPingPeers"));
    g_coreDnsProbe = reinterpret_cast<CoreDnsProbeFn>(dlsym(g_coreHandle, "ArkMeshDnsProbe"));
    g_coreControlDown = reinterpret_cast<CoreControlDownFn>(dlsym(g_coreHandle, "ArkMeshControlDown"));
    g_coreAttachTun = reinterpret_cast<CoreAttachTunFn>(dlsym(g_coreHandle, "ArkMeshAttachTun"));
    g_coreFree = reinterpret_cast<CoreFreeFn>(dlsym(g_coreHandle, "ArkMeshFree"));
    if (g_coreSelfTest == nullptr || g_coreNetCheck == nullptr || g_coreFree == nullptr) {
        return "符号解析失败: ArkMeshSelfTest/ArkMeshNetCheck/ArkMeshFree 不完整";
    }
    return "";
}

// 必须用核心库自己的 free: 每个 Go c-shared 库有独立的分配器堆,
// 拿 libgohello 的 GoFree 去释放这里的指针会跳空指针崩溃。
std::string TakeCoreString(char* s)
{
    if (s == nullptr) {
        return "(Go 返回空指针)";
    }
    std::string out(s);
    g_coreFree(s);
    return out;
}

// ==================== 异步探针包装 ====================
// coreSelfTest/coreNetCheck/coreIfProbe 的阻塞时长以秒计(网络检测串行跑 7 个探针,
// 单个超时 8s; 非法 host 还会卡死在 cgo getaddrinfo 上)。同步跑会把 UI 主线程
// 焊死, 触发 OHOS 的 APP_INPUT_BLOCK(主线程 8s 不响应输入直接杀应用)。
// 所以这三个入口一律返回 Promise, 探针本体丢到 NAPI worker 线程执行。
struct ProbeTask {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    int kind = 0; // 0=自检 1=网络检测 2=接口探测
    std::string host;
    std::string result;
};

void ProbeExecute(napi_env /*env*/, void* data)
{
    // worker 线程: 不许碰任何 napi_* 调用
    ProbeTask* task = static_cast<ProbeTask*>(data);
    switch (task->kind) {
    case 0:
        task->result = "核心加载成功 | " + TakeCoreString(g_coreSelfTest());
        break;
    case 1:
        task->result = TakeCoreString(g_coreNetCheck(const_cast<char*>(task->host.c_str())));
        break;
    case 2:
        if (g_coreIfProbe == nullptr) {
            task->result = "ERR 该核心未导出 ArkMeshIfProbe(需重新编译)";
            break;
        }
        task->result = TakeCoreString(g_coreIfProbe());
        break;
    default:
        task->result = "ERR 未知探针类型";
        break;
    }
    GP_LOGI("异步探针 kind=%{public}d 完成: %{public}s", task->kind, task->result.c_str());
}

void ProbeComplete(napi_env env, napi_status status, void* data)
{
    ProbeTask* task = static_cast<ProbeTask*>(data);
    std::string out = task->result;
    if (status != napi_ok) {
        out = "ERR 异步探针未完成(status=" + std::to_string(static_cast<int>(status)) + ")";
    }
    napi_value value = nullptr;
    napi_create_string_utf8(env, out.c_str(), NAPI_AUTO_LENGTH, &value);
    napi_resolve_deferred(env, task->deferred, value);
    napi_delete_async_work(env, task->work);
    delete task;
}

// 立刻兑现的 Promise: 参数错误也保持 Promise<string> 返回类型一致
napi_value ResolvedPromise(napi_env env, const std::string& s)
{
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    if (napi_create_promise(env, &deferred, &promise) != napi_ok) {
        return MakeString(env, s);
    }
    napi_value value = nullptr;
    napi_create_string_utf8(env, s.c_str(), NAPI_AUTO_LENGTH, &value);
    napi_resolve_deferred(env, deferred, value);
    return promise;
}

napi_value StartProbeAsync(napi_env env, int kind, const std::string& host)
{
    // dlopen / Go 运行时引导仍留在主线程原样完成: 这是先前崩溃调查的基线路径,
    // 不动它, 只把"会阻塞数秒的调用"挪到 worker 线程。
    std::string err = EnsureCoreLoaded();
    if (!err.empty()) {
        GP_LOGE("Go 核心加载失败: %{public}s", err.c_str());
        return ResolvedPromise(env, "ERR " + err);
    }
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    if (napi_create_promise(env, &deferred, &promise) != napi_ok) {
        return MakeString(env, "ERR 创建 Promise 失败");
    }
    ProbeTask* task = new ProbeTask();
    task->deferred = deferred;
    task->kind = kind;
    task->host = host;
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ArkMeshProbe", NAPI_AUTO_LENGTH, &resourceName);
    if (napi_create_async_work(env, nullptr, resourceName, ProbeExecute, ProbeComplete, task,
                               &task->work) != napi_ok) {
        napi_value value = nullptr;
        napi_create_string_utf8(env, "ERR 创建异步任务失败", NAPI_AUTO_LENGTH, &value);
        napi_resolve_deferred(env, deferred, value);
        delete task;
        return promise;
    }
    if (napi_queue_async_work(env, task->work) != napi_ok) {
        napi_delete_async_work(env, task->work);
        napi_value value = nullptr;
        napi_create_string_utf8(env, "ERR 排入异步任务失败", NAPI_AUTO_LENGTH, &value);
        napi_resolve_deferred(env, deferred, value);
        delete task;
        return promise;
    }
    return promise;
}

// 异步: 返回 Promise<string>
napi_value CoreSelfTest(napi_env env, napi_callback_info /*info*/)
{
    return StartProbeAsync(env, 0, "");
}

// args: host — 异步: 返回 Promise<string>
napi_value CoreNetCheck(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return ResolvedPromise(env, "ERR 需要 (host)");
    }
    std::string host = ArgString(env, argv[0]);
    if (host.empty()) {
        return ResolvedPromise(env, "ERR host 为空");
    }
    return StartProbeAsync(env, 1, host);
}

// args: controlURL, authKey, hostname, stateDir, exitNode(空串表示不用出口节点)
napi_value CoreControlUp(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = EnsureCoreLoaded();
    if (!err.empty()) {
        GP_LOGE("Go 核心加载失败: %{public}s", err.c_str());
        return MakeString(env, "ERR " + err);
    }
    if (g_coreControlUp == nullptr) {
        return MakeString(env, "ERR 该核心未导出 ArkMeshControlUp(需重新编译)");
    }
    if (argc < 5) {
        return MakeString(env, "ERR 需要 (controlURL, authKey, hostname, stateDir, exitNode)");
    }
    std::string controlURL = ArgString(env, argv[0]);
    std::string authKey = ArgString(env, argv[1]);
    std::string hostname = ArgString(env, argv[2]);
    std::string stateDir = ArgString(env, argv[3]);
    std::string exitNode = ArgString(env, argv[4]);
    std::string rc = TakeCoreString(g_coreControlUp(
        const_cast<char*>(controlURL.c_str()), const_cast<char*>(authKey.c_str()),
        const_cast<char*>(hostname.c_str()), const_cast<char*>(stateDir.c_str()),
        const_cast<char*>(exitNode.c_str())));
    GP_LOGI("CoreControlUp control=%{public}s host=%{public}s exit=%{public}s rc=%{public}s",
            controlURL.c_str(), hostname.c_str(),
            (exitNode.empty() ? "(none)" : exitNode.c_str()), rc.c_str());
    return MakeString(env, rc);
}

// 异步: 返回 Promise<string>
napi_value CoreIfProbe(napi_env env, napi_callback_info /*info*/)
{
    return StartProbeAsync(env, 2, "");
}

napi_value CoreControlStatus(napi_env env, napi_callback_info info)
{
    std::string err = EnsureCoreLoaded();
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (g_coreControlStatus == nullptr) {
        return MakeString(env, "ERR 该核心未导出 ArkMeshControlStatus(需重新编译)");
    }
    return MakeString(env, TakeCoreString(g_coreControlStatus()));
}

// args: type ("TSMP" / "ICMP" / "disco", 空串按 TSMP)
// 让 Go 核心在后台对在线 peer 发隧道内 ping。系统 ping 在这里没用: OHOS 的 VPN
// 按 UID 抓流量, hdc shell 不在范围内, 扩展进程自己又被 protectProcessNet 绕过了。
// 立即返回, 结果通过 CoreControlStatus 的 pings / pingBusy 字段读。
napi_value CorePingPeers(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = EnsureCoreLoaded();
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (g_corePingPeers == nullptr) {
        return MakeString(env, "ERR 该核心未导出 ArkMeshPingPeers(需重新编译)");
    }
    std::string type = (argc >= 1) ? ArgString(env, argv[0]) : std::string("TSMP");
    std::string rc = TakeCoreString(g_corePingPeers(const_cast<char*>(type.c_str())));
    GP_LOGI("CorePingPeers type=%{public}s rc=%{public}s", type.c_str(), rc.c_str());
    return MakeString(env, rc);
}

// args: name(空串=自动挑目标), timeoutMs(<=0 按 3000)
// 同步在隧道内注入合成 MagicDNS 查询并等回包, 返回一行可读结果。
// 必须在隧道 state=="up" 之后调, 否则 Go 侧直接返回 "ERR 隧道未就绪"。
napi_value CoreDnsProbe(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = EnsureCoreLoaded();
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (g_coreDnsProbe == nullptr) {
        return MakeString(env, "ERR 该核心未导出 ArkMeshDnsProbe(需重新编译)");
    }
    std::string name = (argc >= 1) ? ArgString(env, argv[0]) : std::string("");
    int32_t timeoutMs = (argc >= 2) ? ArgInt(env, argv[1]) : 3000;
    std::string rc = TakeCoreString(g_coreDnsProbe(const_cast<char*>(name.c_str()), timeoutMs));
    GP_LOGI("CoreDnsProbe name=%{public}s rc=%{public}s",
            (name.empty() ? "(auto)" : name.c_str()), rc.c_str());
    return MakeString(env, rc);
}

napi_value CoreControlDown(napi_env env, napi_callback_info info)
{
    std::string err = EnsureCoreLoaded();
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (g_coreControlDown != nullptr) {
        g_coreControlDown();
    }
    GP_LOGI("CoreControlDown 已调用");
    return MakeString(env, "ok");
}

// args: fd, mtu
// 把 VpnExtensionAbility 建好的 VPN fd 交给 Go 核心, 让它带 TUN 重建 tsnet。
// 返回空串=已受理(重建在 Go 侧后台跑, 用 CoreControlStatus 轮询), "ERR ..."=失败。
napi_value CoreAttachTun(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string err = EnsureCoreLoaded();
    if (!err.empty()) {
        return MakeString(env, "ERR " + err);
    }
    if (g_coreAttachTun == nullptr) {
        return MakeString(env, "ERR 该核心未导出 ArkMeshAttachTun(需重新编译)");
    }
    if (argc < 1) {
        return MakeString(env, "ERR 需要 (fd, mtu)");
    }
    int32_t fd = ArgInt(env, argv[0]);
    int32_t mtu = (argc >= 2) ? ArgInt(env, argv[1]) : 1420;
    std::string rc = TakeCoreString(g_coreAttachTun(fd, mtu));
    GP_LOGI("CoreAttachTun fd=%{public}d mtu=%{public}d rc=%{public}s",
            fd, mtu, rc.c_str());
    return MakeString(env, rc);
}

} // namespace gobridge
