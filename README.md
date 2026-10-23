# libmpclient v2 (rewrite)

Drop-in replacement for the loader's `libmpclient.so` (arm64-v8a). Same files and config as before
(`mp.cfg`, `mp_log.txt`, `mp_say.txt`, `mp_status.txt`, `Mods/`), but speaks protocol v2 with the v2 server (`server/`, see `server/PROTOCOL.md`).

What it syncs: players (position, movement, aiming/shooting, crouch, ragdoll, death, vehicles), clothes + props, weapons,
animations (`/anim <dict> <clip> [flags]`, `/stopanim` typed through mp_say.txt), shared time of day + weather, and the host's NPC pedestrians and traffic.
Host commands (chat or server console): `/time 23 30`, `/weather RAIN`, `/freeze`, `/speed 30`.

## Build
GitHub Actions does it: push this folder to a repo, run the `build-libmpclient` workflow, download the `libmpclient-arm64-v8a` artifact.
Local: `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static && cmake --build build`.
Host test (no phone needed): `g++ -std=c++17 -pthread test/host_test.cpp src/client.cpp src/client_game.cpp -o /tmp/host_test && SERVER_DIR=$PWD/server sh test/run_host_test.sh`.

## Install
Back up the original `lib/arm64-v8a/libmpclient.so` of the APK, replace it with the new one, re-sign, install. Keep `mp.cfg` as is.

## Debugging
`mp_log.txt` shows: `hook: installed`, `trainer snapshot: {...}` (first one, shows the real JSON), `selftest: ped=... pos=...` (proves natives work and returns sane values),
`joined as #N (protocol v2)`, `spawned ped ...`. If a line says `tick commands are not reaching the hook`, or `selftest` shows nonsense, send the log.
`src/natives.h` lists every native with the hash of this game build; `RE_NOTES.md` documents everything taken from the old client.
