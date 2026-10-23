# Reverse-engineering notes (libmpclient.so v1 -> this rewrite)

Game lib: libgtav.so (build = FiveM crossmap column 25). All offsets are file/vaddr offsets from the lib load bias.

## Hook (phase 2)
- target fn  = libgtav+0x21439e4  prologue (16 B): ea 0f 18 fc | e9 23 01 6d | fd 7b 02 a9 | fc 6f 03 a9
- g_setResult = libgtav+0x2143290 (prologue stp x29,x30,[sp,-0x20]!; str x19,[sp,0x10]; mov x29,sp; adrp ...)
  signature (obj, int cmdId, int ok, const char* msg); obj = *(libgtav+0x3b3f780)... NOTE: g_resultObj = (base + 0x3b3f780) itself (address, not deref)
- g_native    = libgtav+0x2df654c (stp x29,x30,[sp,-0x20]!; str x19,[sp,0x10]; mov x29,sp; mov x19,x1)
  signature bool g_native(uint64 hash, Ctx*), Ctx = { u64* ret; i32 argc; i32 pad; u64* args; i32 dataCount; } (args are u64 slots, floats = bits in low 32)
  ret buffer 4+ qwords zeroed, Vector3 result = 3 qwords (x,y,z at 8-byte stride)
- patch: mmap 0x1000 RW, copy 16 orig bytes, then `ldr x16,#8 (0x58000050); br x16 (0xd61f0200); .quad target+0x10`, mprotect RX.
  target gets `ldr x16,#8; br x16; .quad hook_execute` (mprotect page RWX, write, clear cache, RX).
- hook_execute(Entry* e): if e && e->type(i32 @0)==0x33 && *(u64*)(e+0x10)==0x00636e79737 06d40 ("@mpsync\0") -> run tick on game thread,
  then g_setResult(g_resultObj, e->id (i32 @4), 1, "mp sync"), return e. else tail-call original (trampoline).
- libgtav bias via dl_iterate_phdr (name contains "libgtav.so"). Natives table dump (mp_natives.txt) at bias+0x325ae00 (not needed any more).

## Trainer JNI (find with dlsym RTLD_DEFAULT, fallback dlopen(NOLOAD) libgtav.so / libSDL2.so / libmain.so)
- Java_com_gtavsource_android_TrainerBridge_nativeSetEnabled(env, cls, jboolean)   -> call with 1
- Java_com_gtavsource_android_TrainerBridge_nativeSubmit(env, cls, jstring cmd, jstring text, jint a, jint b, jfloatArray f) -> jint id (>=1 ok)
- Java_com_gtavsource_android_TrainerBridge_nativeSnapshot(env, cls) -> jstring JSON (keys seen: epoch, busy, completed, pending?, head, heal(th), vehicle, entities, model)
- env = SDL_AndroidGetJNIEnv()
- tick trigger: submit("ped.stop", "@mpsync", a=0, b=epoch). <=2 in flight; interval 16 ms (when remotes exist) else 50 ms; hook counter vs submit counter.
  If hook counter stays 0 for 3 s: "tick commands are not reaching the hook".

## Files / config
- /storage/emulated/0/Games/GTAV/{mp.cfg (server=ip[:port], name=, port=, http=), mp_log.txt, mp_say.txt (chat lines, consumed), mp_status.txt, mp_natives.txt}
- /storage/emulated/0/Games/GTAV/Mods/{inbox/, mp_installed.txt ("name\tsha256")}
- v1 protocol: join {"t":"join","name","v":1,"mods":setHash}, ping {"t":"ping"} 1/s, state {"t":"s",...}; join retry 2 s; server silence 10 s => rejoin.
