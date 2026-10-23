// End-to-end host test: two real clients <-> real server (node), each with a simulated game world.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/client.h"
#include "mock_game.h"
using namespace mp;
static int pass = 0, fail = 0;
#define CHECK(c, msg) do { bool ok_ = (c); ok_ ? pass++ : fail++; printf("%s %s\n", ok_ ? "PASS" : "FAIL", msg); fflush(stdout); } while (0)
static void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::milliseconds((int)(s * 1000))); }
static void writeFile(const std::string& p, const std::string& s) { FILE* f = fopen(p.c_str(), "w"); fputs(s.c_str(), f); fclose(f); }
struct Driver { MockGame& g; Client& c; std::atomic<bool> run{true}; std::thread t; Driver(MockGame& g_, Client& c_) : g(g_), c(c_) { t = std::thread([this] { while (run) { g.step(0.016f); c.tick(g); sleep_s(0.016); } }); } ~Driver() { run = false; if (t.joinable()) t.join(); } };

int main() {
  set_log("/tmp/mp_host_test.log", [](const std::string& s) { printf("   [log] %s\n", s.c_str()); });
  unlink("/tmp/mp_host_test.log");
  for (const char* d : {"/tmp/mpA", "/tmp/mpB"}) { mkdir(d, 0775); unlink((std::string(d) + "/mp_say.txt").c_str()); }
  writeFile("/tmp/mpA/mp.cfg", "server=127.0.0.1\nname=Alice\n"); writeFile("/tmp/mpB/mp.cfg", "server=127.0.0.1:30120\nname=Bob\n");
  const uint32_t FREEMODE = joaat("mp_m_freemode_01"), PISTOL = 0x1B06D571u, SULTAN = joaat("sultan"), HIP = joaat("a_f_y_hipster_01"), BIZ = joaat("a_m_m_business_01"), BEACH = joaat("a_m_y_beach_01");
  MockGame A, B;
  { MEnt& p = A.player(); p.model = FREEMODE; p.pos = {100, 200, 30}; p.weapon = PISTOL; p.comps[3][0] = 5; p.comps[3][1] = 2; p.comps[11][0] = 22; p.comps[11][1] = 1; p.props[0][0] = 12; p.props[0][1] = 3; }
  auto amb = [&](int type, uint32_t m, Vec3 pos) { MEnt e; e.type = type; e.model = m; e.pos = pos; e.ambient = true; int h = A.nextHandle++; A.ents[h] = e; return h; };
  amb(1, HIP, {110, 200, 30}); amb(1, BIZ, {95, 205, 30}); amb(1, BEACH, {900, 900, 30}); int ambVeh = amb(2, SULTAN, {105, 210, 30}); A.ents[ambVeh].col[0] = 12; A.ents[ambVeh].col[1] = 34;
  int alicesCar = amb(2, SULTAN, {101, 200, 30}); A.ents[alicesCar].ambient = false; A.ents[alicesCar].pos = {5000, 5000, 30}; // parked far away until Alice gets in
  { MEnt& p = B.player(); p.model = joaat("player_zero"); p.pos = {110, 210, 30}; }
  int bAmb = 0; { MEnt e; e.type = 1; e.model = HIP; e.pos = {112, 212, 30}; e.ambient = true; bAmb = B.nextHandle++; B.ents[bAmb] = e; }   // Bob's own ambient ped: must be purged once Alice owns the area
  Env eA, eB; eA.dir = "/tmp/mpA"; eB.dir = "/tmp/mpB";
  Client cA(eA), cB(eB);
  std::thread nA([&] { cA.run(); });
  Driver dA(A, cA);
  sleep_s(1.5);
  std::thread nB([&] { cB.run(); });
  Driver dB(B, cB);
  A.player().vel = {3, 0, 0}; // Alice walks east
  for (int i = 0; i < 100 && !(cA.joined() && cB.joined()); i++) sleep_s(0.1);
  CHECK(cA.joined() && cB.joined(), "both clients joined the server");
  CHECK(cA.hostId() == cA.myId() && cB.hostId() == cA.myId(), "Alice (first) is the world host for both");
  sleep_s(5.0);

  // ---- phase 1: look, weapon, movement, world, population, NPCs
  int puppet = 0; for (int h : B.puppetsOf(1)) if (B.ents[h].model == FREEMODE) puppet = h;
  CHECK(puppet != 0, "Bob spawned a puppet ped with Alice's model");
  if (puppet) {
    MEnt pe; { std::lock_guard<std::mutex> l(B.mu); pe = B.ents[puppet]; }
    Vec3 ap = A.posOf(A.playerHandle); float d = std::hypot(pe.pos.x - ap.x, pe.pos.y - ap.y);
    CHECK(d < 6.0f, "puppet follows Alice (<6 m behind while she walks)");
    CHECK(pe.comps[3][0] == 5 && pe.comps[3][1] == 2 && pe.comps[11][0] == 22 && pe.comps[11][1] == 1, "clothes copied (components 3 and 11)");
    CHECK(pe.props[0][0] == 12 && pe.props[0][1] == 3 && pe.props[1][0] == -1, "props copied (hat on, others cleared)");
    CHECK(pe.weapon == PISTOL, "weapon copied (pistol)");
  }
  CHECK(B.clockPaused && B.clockH == 22 && B.clockSets > 20, "Bob's clock follows the server: night (22:xx), paused locally");
  CHECK(B.weather == "CLEAR", "Bob's weather follows the server");
  CHECK(A.clockPaused && A.clockH == 22, "Alice also on server time");
  CHECK(!B.ents[bAmb].exists, "Bob's own ambient ped purged (area owned by Alice, no duplicates)");
  CHECK(B.pedBudget == 0 && B.vehBudget == 0 && B.densityCalls > 50, "Bob's local traffic suppressed (mirrors the host)");
  CHECK(A.pedBudget == -1 && A.densityCalls == 0, "Alice (host) keeps normal traffic");
  { auto peds = B.puppetsOf(1), vehs = B.puppetsOf(2); bool hip = false, biz = false, beach = false; for (int h : peds) { uint32_t m = B.ents[h].model; hip |= m == HIP; biz |= m == BIZ; beach |= m == BEACH; }
    CHECK(hip && biz, "host NPCs mirrored on Bob (2 pedestrians)");
    CHECK(!beach, "NPC 1 km away not sent (interest radius)");
    CHECK(vehs.size() == 1 && B.ents[vehs[0]].model == SULTAN && B.ents[vehs[0]].col[0] == 12 && B.ents[vehs[0]].col[1] == 34, "host car mirrored with its colours"); }
  { int n = 0; for (int h : A.puppetsOf(1)) { (void)h; n++; } CHECK(n == 1 && A.puppetsOf(2).empty(), "Alice only has a puppet of Bob, none of the mirrored world"); }

  // ---- phase 2: host commands + animation
  writeFile("/tmp/mpA/mp_say.txt", "/time 5 30\n/weather RAIN\n/anim amb@world_human_hang_out_street@male_a@idle_a idle_a 1\n");
  sleep_s(3.0);
  CHECK(B.clockH == 5 && B.clockM >= 30 && B.clockM <= 33, "host /time 5 30 reached Bob (05:3x)");
  CHECK(B.weather == "RAIN", "host /weather RAIN reached Bob");
  CHECK(B.animCalls >= 1 && B.lastAnimDict == "amb@world_human_hang_out_street@male_a@idle_a" && B.lastAnimClip == "idle_a", "animation relayed and played on Alice's puppet");
  CHECK(A.animCalls >= 1, "Alice played the animation locally too");
  writeFile("/tmp/mpB/mp_say.txt", "/time 12 0\n"); sleep_s(1.0);
  CHECK(A.clockH == 5, "non-host /time ignored");

  // ---- phase 3: aiming + shooting, crouch
  A.player().vel = {0, 0, 0}; A.flags = 1 | 2 | 8; int s0 = B.shootCalls; sleep_s(1.5);
  CHECK(B.shootCalls > s0, "puppet shoots when Alice shoots");

  // ---- phase 3b: chat on screen + shared wanted level
  { int f0 = B.feedPosts; writeFile("/tmp/mpA/mp_say.txt", "hello from alice\n"); sleep_s(1.5); CHECK(B.feedPosts > f0, "chat message shown on Bob's screen (feed ticker)"); }
  A.wanted = 3; sleep_s(2.5); CHECK(B.wanted == 3, "wanted level of Alice reached Bob");
  B.wanted = 0; sleep_s(2.5); CHECK(A.wanted == 0, "Bob clearing it clears it for Alice");

  // ---- phase 4: Alice drives
  A.flags = 0; { MEnt& c = A.ents[alicesCar]; std::lock_guard<std::mutex> l(A.mu); c.pos = A.player().pos; c.pos.x += 0.5f; A.player().veh = alicesCar; A.player().seat = -1; c.vel = {10, 0, 0}; A.player().vel = {10, 0, 0}; }
  sleep_s(3.0);
  { auto vehs = B.puppetsOf(2); int driven = 0; for (int h : vehs) for (auto& kv : B.ents) if (kv.second.puppet && kv.second.type == 1 && kv.second.veh == h && kv.second.seat == -1) driven = h;
    CHECK(driven != 0 && B.ents[driven].model == SULTAN, "Alice's puppet sits in a mirrored car as driver");
    CHECK(vehs.size() == 2, "no duplicate: host's own car is not also streamed as an NPC car");
    if (driven) { Vec3 cp = B.posOf(driven), ap = A.posOf(A.playerHandle); CHECK(std::hypot(cp.x - ap.x, cp.y - ap.y) < 12.0f, "puppet car stays near Alice's real car"); } }

  // ---- phase 5: host leaves
  cA.stop(); nA.join(); dA.run = false; dA.t.join();
  sleep_s(5.0);
  CHECK(B.puppetsOf(1).empty() && B.puppetsOf(2).empty(), "everything of the departed host removed on Bob");
  CHECK(cB.hostId() == cB.myId() && B.pedBudget == 3 && B.vehBudget == 3, "Bob became host: local traffic restored");
  CHECK(A.unknown.empty() && B.unknown.empty(), "client only calls natives the mock knows (no unexpected natives)");
  if (!A.unknown.empty() || !B.unknown.empty()) for (auto h : B.unknown) printf("   unknown native %016llx\n", (unsigned long long)h);
  cB.stop(); nB.join(); dB.run = false; dB.t.join();
  printf("\n%d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
