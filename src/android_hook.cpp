// Android glue: engine hook (libgtav.so), trainer JNI channel, native-call backend and library boot.
// Every constant below comes from the disassembly of the previous libmpclient.so (see RE_NOTES.md).
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "client.h"

using namespace mp;
namespace {
constexpr uintptr_t OFF_EXEC = 0x21439e4, OFF_SETRESULT = 0x2143290, OFF_NATIVE = 0x2df654c, OFF_RESULTOBJ = 0x3b3f780;
constexpr uint64_t MAGIC = 0x00636e7973706d40ULL;   // "@mpsync\0"
struct Entry { int32_t type; int32_t id; uint64_t pad; uint64_t name; };
using ExecFn = Entry* (*)(Entry*);
using NativeFn = bool (*)(uint64_t, void*);
using SetResultFn = void (*)(void*, int, int, const char*);
ExecFn g_orig = nullptr; NativeFn g_native = nullptr; SetResultFn g_setResult = nullptr; void* g_resultObj = nullptr;
std::atomic<uint64_t> g_ticks{0}; Client* g_client = nullptr; std::atomic<bool> g_hooked{false};

struct NativeCtx { uint64_t* ret; int32_t argc; int32_t pad; uint64_t* args; int32_t dataCount; int32_t pad2; uint64_t spare[24]; };
class EngineNatives : public Natives {
 public:
  bool call(uint64_t hash, std::initializer_list<uint64_t> a, uint64_t* ret) override {
    uint64_t argv[32]; memset(argv, 0, sizeof argv); int n = 0; for (uint64_t v : a) if (n < 32) argv[n++] = v;
    NativeCtx c; memset(&c, 0, sizeof c); c.ret = ret; c.argc = n; c.args = argv;
    return g_native(hash, &c);
  }
};

// Runs on the game's trainer thread whenever the engine executes a command; our "@mpsync" ped.stop command is the tick.
extern "C" __attribute__((used, noinline)) Entry* mp_hook_execute(Entry* e) {
  if (e && e->type == 0x33 && e->name == MAGIC) {
    if (g_client) { EngineNatives n; g_client->tick(n); }
    g_ticks++;
    g_setResult(g_resultObj, e->id, 1, "mp sync");
    return e;
  }
  return g_orig(e);
}

int find_bias_cb(dl_phdr_info* i, size_t, void* d) { if (i->dlpi_name && strstr(i->dlpi_name, "libgtav.so")) { *(uintptr_t*)d = i->dlpi_addr; return 1; } return 0; }
bool words_match(const void* p, const uint32_t (&w)[4]) { return memcmp(p, w, 16) == 0; }

bool install_hook() {
  uintptr_t base = 0; dl_iterate_phdr(find_bias_cb, &base);
  if (!base) { logf("hook: libgtav.so not loaded yet"); return false; }
  uint32_t* exec = (uint32_t*)(base + OFF_EXEC);
  static const uint32_t sigExec[4] = {0xfc180feau, 0x6d0123e9u, 0xa9027bfdu, 0xa9036ffcu};
  static const uint32_t sigSet[4] = {0xa9be7bfdu, 0xf9000bf3u, 0x910003fdu, 0x9000cfe8u};
  static const uint32_t sigNat[4] = {0xa9be7bfdu, 0xf9000bf3u, 0x910003fdu, 0xaa0103f3u};
  if (!words_match(exec, sigExec)) { logf("hook: signature mismatch at libgtav+0x%lx (different game build?)", (unsigned long)OFF_EXEC); return false; }
  if (!words_match((void*)(base + OFF_SETRESULT), sigSet) || !words_match((void*)(base + OFF_NATIVE), sigNat)) { logf("hook: native invoker signature mismatch"); return false; }
  size_t pg = (size_t)sysconf(_SC_PAGESIZE);
  void* tramp = mmap(nullptr, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tramp == MAP_FAILED) { logf("hook: mmap failed"); return false; }
  uint32_t* t = (uint32_t*)tramp; memcpy(t, exec, 16); t[4] = 0x58000050u /* ldr x16,#8 */; t[5] = 0xd61f0200u /* br x16 */;
  uint64_t back = (uint64_t)(uintptr_t)(exec + 4); memcpy(t + 6, &back, 8);
  __builtin___clear_cache((char*)tramp, (char*)tramp + 32); mprotect(tramp, pg, PROT_READ | PROT_EXEC);
  g_orig = (ExecFn)tramp; g_native = (NativeFn)(base + OFF_NATIVE); g_setResult = (SetResultFn)(base + OFF_SETRESULT); g_resultObj = (void*)(base + OFF_RESULTOBJ);
  uintptr_t page = (uintptr_t)exec & ~(uintptr_t)(pg - 1); size_t len = ((uintptr_t)exec + 16 > page + pg) ? 2 * pg : pg;
  if (mprotect((void*)page, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) { logf("hook: mprotect failed"); return false; }
  uint64_t h = (uint64_t)(uintptr_t)&mp_hook_execute; memcpy(exec + 2, &h, 8);
  exec[0] = 0x58000050u; exec[1] = 0xd61f0200u;
  __builtin___clear_cache((char*)exec, (char*)exec + 16); mprotect((void*)page, len, PROT_READ | PROT_EXEC);
  logf("hook: installed at libgtav+0x%lx", (unsigned long)OFF_EXEC);
  return true;
}

// ---- trainer JNI channel
void* find_sym(const char* name) {
  if (void* p = dlsym(RTLD_DEFAULT, name)) return p;
  for (const char* lib : {"libgtav.so", "libSDL2.so", "libmain.so"}) if (void* h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD)) if (void* p = dlsym(h, name)) return p;
  return nullptr;
}
struct Trainer {
  JNIEnv* env = nullptr;
  jint (*submit)(JNIEnv*, jclass, jstring, jstring, jint, jint, jfloatArray) = nullptr;
  jstring (*snap)(JNIEnv*, jclass) = nullptr; void (*enable)(JNIEnv*, jclass, jboolean) = nullptr;
  bool init() {
    submit = (decltype(submit))find_sym("Java_com_gtavsource_android_TrainerBridge_nativeSubmit");
    snap = (decltype(snap))find_sym("Java_com_gtavsource_android_TrainerBridge_nativeSnapshot");
    enable = (decltype(enable))find_sym("Java_com_gtavsource_android_TrainerBridge_nativeSetEnabled");
    auto getEnv = (void* (*)())find_sym("SDL_AndroidGetJNIEnv");
    if (!submit || !snap || !enable || !getEnv) return false;
    env = (JNIEnv*)getEnv(); if (!env) return false;
    enable(env, nullptr, JNI_TRUE); return true;
  }
  int submitCmd(const char* cmd, const char* text, int a, int b) {
    jstring jc = env->NewStringUTF(cmd), jt = env->NewStringUTF(text); jfloatArray fa = env->NewFloatArray(0);
    jint r = submit(env, nullptr, jc, jt, a, b, fa);
    if (env->ExceptionCheck()) { env->ExceptionClear(); r = -1; }
    env->DeleteLocalRef(jc); env->DeleteLocalRef(jt); env->DeleteLocalRef(fa); return r;
  }
  int epoch() {
    jstring s = snap(env, nullptr); if (env->ExceptionCheck()) { env->ExceptionClear(); return last; } if (!s) return last;
    const char* c = env->GetStringUTFChars(s, nullptr); int e = last;
    if (c) { static bool logged = false; if (!logged) { logged = true; logf("trainer snapshot: %.300s", c); } JV j; if (JParser::parse(c, strlen(c), j) && j.t == JV::Obj) { e = j.at("epoch").i(last); } env->ReleaseStringUTFChars(s, c); }
    env->DeleteLocalRef(s); last = e; return e;
  }
  int last = 0;
};

void* worker(void*) {
  Env env; unlink((env.dir + "/mp_log.txt").c_str());
  set_log(env.dir + "/mp_log.txt", [](const std::string& s) { __android_log_print(ANDROID_LOG_INFO, "mpclient", "%s", s.c_str()); });
  logf("libmpclient loaded");
  if (access((env.dir + "/mp.cfg").c_str(), R_OK) != 0) { logf("no %s/mp.cfg (server=...) -> inactive", env.dir.c_str()); return nullptr; }
  static Trainer tr; bool trainerOk = false, hookOk = false;
  for (int i = 0; i < 240 && !(trainerOk && hookOk); i++) {      // wait (up to 2 min) for the game libraries to come up
    if (!trainerOk) trainerOk = tr.init();
    if (trainerOk && !hookOk) hookOk = install_hook();
    if (!(trainerOk && hookOk)) usleep(500000);
  }
  if (!trainerOk) logf("trainer exports not found; world sync disabled");
  if (trainerOk && hookOk) { g_hooked = true; logf("mode: phase 2 (engine hook)"); }
  env.tickReady = [] { return g_hooked.load(); };
  env.ticksDone = [] { return g_ticks.load(); };
  if (g_hooked) {
    env.epoch = [] { return tr.epoch(); };
    env.requestTick = [] {
      static double t0 = now_ms(); static int mode = 0; static double lastSwitch = now_ms();
      double now = now_ms();
      if (g_ticks == 0 && now - lastSwitch > 4000) { mode = (mode + 1) % 3; lastSwitch = now; logf("tick: no hook callback yet, trying epoch argument mode %d", mode); }
      (void)t0; int b = mode == 0 ? tr.last : (mode == 1 ? 0 : 1);
      return tr.submitCmd("ped.stop", "@mpsync", 0, b) >= 1;
    };
  }
  static Client client(env); g_client = &client;
  client.run();
  return nullptr;
}
__attribute__((constructor)) void mp_boot() { pthread_t t; if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t); }
}  // namespace
