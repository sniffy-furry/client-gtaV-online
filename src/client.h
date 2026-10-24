// libmpclient v2: platform independent client logic (network thread + game-thread tick).
#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "game.h"
#include "json.h"

namespace mp {
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void set_log(const std::string& path, std::function<void(const std::string&)> sink);
double now_ms();

struct Env {
  std::string dir = "/storage/emulated/0/Games/GTAV";       // mp.cfg, mp_log.txt, mp_say.txt, Mods/
  std::function<bool()> requestTick;                         // Android: submit the trainer command that runs tick() in the engine hook (true = queued)
  std::function<int()> epoch;                                // Android: scene epoch from the trainer snapshot (nullptr = unused)
  std::function<uint64_t()> ticksDone;                       // Android: how many ticks the hook executed
  std::function<bool()> tickReady;                           // Android: hook + trainer available
};

struct PState { int id = 0; std::string name; float x = 0, y = 0, z = 0, h = 0, vx = 0, vy = 0, vz = 0, cp = 0, cy = 0; std::vector<float> vd; std::string vdj; int rv = 0, rg = 0; int hp = 200; uint32_t wp = 0; int iv = 0; std::string vm; int fl = 0, sd = -2, am = 0; };
struct App { bool valid = false, dirty = false; uint32_t model = 0; std::vector<int> c, p; };
struct Loadout { bool valid = false, dirty = false; uint32_t w = 0; int am = 0; };
struct ERow { int g = 0, k = 1; uint32_t m = 0; float x = 0, y = 0, z = 0, h = 0, vx = 0, vy = 0, vz = 0; int fl = 0, a = 0, b = 0, ctl = 0; std::vector<float> d; };
struct AnimEv { int id; std::string dict, clip; int flags; bool start; };

class Client {
 public:
  explicit Client(const Env& env) : env_(env) {}
  void run();                 // network thread; returns after stop()
  void stop() { stop_ = true; }
  void tick(Natives& n);      // game thread (inside the engine hook)
  // observability for tests/status
  bool joined() const { return joined_; }
  int myId() const { return myId_; }
  int hostId() const { return hostId_; }
  struct Stats { std::atomic<int> puppetPlayers{0}, puppetEnts{0}, ticks{0}; };
  Stats stats;

 private:
  Env env_;
  std::atomic<bool> stop_{false}, joined_{false};
  std::atomic<int> myId_{0}, hostId_{0}, epoch_{0};
  std::atomic<bool> popOwn_{true};   // true: my game runs the ambient population around me and streams it; false: another player owns this area, I only mirror
  std::atomic<bool> epochChanged_{false};

  // ---- config / mods / network (net thread)
  struct Cfg { std::string host; int port = 30120, http = 0; std::string name = "Player"; bool ok = false; } cfg_;
  bool loadCfg();
  bool syncMods(std::string& modsField);
  bool openUdp();
  void sendRaw(const std::string& s);
  void handle(const std::string& line);
  void sayFile();
  void writeStatus(bool force);
  int sock_ = -1;
  double lastRx_ = 0, lastJoin_ = 0, lastTx_ = 0, lastStateTx_ = 0, lastStatus_ = 0, lastTickReq_ = 0, lastTickWarn_ = 0, waitSince_ = 0;
  uint64_t tickReqCount_ = 0;
  std::string modsField_;
  int serverTickHz_ = 10, entRate_ = 6, interest_ = 300; std::string pop_ = "host";
  bool v2_ = false, tickWarned_ = false;

  // ---- shared between net thread and game thread
  struct Shared {
    std::mutex mu;
    // net -> game
    std::vector<PState> players; bool playersNew = false;
    std::map<int, App> apps; std::map<int, Loadout> loads; std::vector<AnimEv> anims;
    std::vector<ERow> ents; std::vector<int> edel; std::vector<std::pair<int, int>> owns; std::vector<int> left, take, wlIn; std::vector<std::string> chatIn;
    bool worldNew = false; int wsec = 0; float wscale = 30; bool wfrozen = false; std::string wweather = "CLEAR"; double wrecv = 0;
    std::vector<std::string> localAnim; // "dict|clip|flags" or "stop"
    // game -> net
    bool localValid = false; double localAt = 0; PState local;
    std::vector<std::string> outbox;
  } sh_;

  // ---- game thread state
  struct Remote {
    PState st; double lastSeen = 0, spawnAt = 0, lastTask = 0, lastCoordSet = 0; bool seen = false;
    std::string animDict, animClip; int animFlags = 0; bool animPending = false, animStop = false; double animSince = 0;
    App app; Loadout load; double vehOutSince = 0; int riding = 0; std::vector<float> vdApplied; int vbits = -1; int ped = 0, veh = 0; uint32_t pedModel = 0, vehModel = 0, curWeapon = 0xFFFFFFFFu; int lastFl = 0; bool moving = false, dead = false, crouch = false, appliedApp = false;
  };
  struct EPup { ERow row; std::vector<float> d; bool dNew = false, dead = false; int vbits = -1, lastFl = 0; uint32_t wep = 0xFFFFFFFFu; double lastSeen = 0, spawnAt = 0, lastTask = 0; int handle = 0; bool moving = false, seated = false; };
  struct VdC { int slow = 0; double at = -1e9; };
  std::map<int, VdC> vdc_; std::map<int, double> pedDetAt_; std::string localVd_; double lastVdAt_ = 0;
  std::map<int, Remote> remotes_; std::map<int, EPup> eps_;
  std::map<int, int> lidOf_; std::map<int, double> lidSeen_; int nextLid_ = 1;    // host streaming: entity handle -> local id
  std::set<int> ownedByUs_; std::map<int, double> xferred_; std::vector<int> killLids_; std::set<int> taken_; double lastPurge_ = 0; int appliedWl_ = 0; double wlSetAt_ = 0, lastWlPoll_ = 0; double lastStreamLog_ = 0;                                                       // handles of our own puppets (never stream them)
  struct World { bool valid = false; double sec = 0, recv = 0; float scale = 30; bool frozen = false; std::string weather; } world_;
  double lastClock_ = 0; int lastLocalPed_ = 0; std::string pDict_, pClip_; int pFlags_ = 0; double pSince_ = 0;
  int lastPed_ = 0; bool selfTested_ = false, clockPaused_ = false, popSuppressed_ = false;
  double lastSample_ = 0, lastFlags_ = 0, lastApp_ = 0, lastLoad_ = 0, lastStream_ = 0, lastAppSent_ = 0, lastLoadSent_ = 0; std::string lastAppJson_;
  uint32_t lastSentW_ = 0xFFFFFFFFu; int lastSentAm_ = -1; std::string appliedWeather_;
  int localFl_ = 0, localSd_ = -2, localAm_ = 0;

  void selfTest(Game& g, int me);
  void sampleLocal(Game& g, int ped, double now);
  void applyInbox(double now);
  void applyWorld(Game& g, double now);
  void applyChat(Game& g);
  int vehicleHandleFor(Game& g, int me, int rv, int rg);
  std::string vehDetail(Game& g, int h, double now);
  void applyVehDetail(Game& g, int veh, const std::vector<float>& d, int& lastBits);
  void purgeAmbient(Game& g, int me, double now);
  void applyWanted(Game& g, double now);
  bool dupOfPlayerCar(const Vec3& pos, uint32_t model) const;
  void applyPopulation(Game& g, double now);
  void updatePlayers(Game& g, int me, double now);
  void updateEntities(Game& g, int me, double now);
  void streamEntities(Game& g, int me, double now);
  void dropPuppets(Game& g, bool engineGone);
  void moveActor(Game& g, int ent, const Vec3& target, const Vec3& vel, float heading, double now, double& lastTask, bool& moving, bool aiming);
  void applyAppearance(Game& g, Remote& r);
  void applyWeapon(Game& g, Remote& r);
  void queueOut(const std::string& s);
};
}  // namespace mp
