// Game-thread half of the client: runs inside the engine hook, talks to the game only through Game/Natives.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "client.h"

namespace mp {
static const float kPi = 3.14159265f;
static const uint32_t kUnarmed = 0xA2719263u;
static std::string fmt(const char* f, ...) { char b[1500]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap); return b; }
static uint32_t parseHash(const std::string& s) { return (uint32_t)strtoul(s.c_str(), nullptr, 10); }
// puppets only show what their owner's game says: no local bullet/fire/explosion/collision damage on them
static void proofOn(Game& g, int h, bool on) { g.V(nat::SET_ENTITY_PROOFS, {A(h), A(on), A(on), A(on), A(on), A(on), A(on), A(on), A(on)}); }

static Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
enum Fl { F_AIM = 1, F_SHOOT = 2, F_RELOAD = 4, F_CROUCH = 8, F_RAGDOLL = 16, F_SPRINT = 32, F_VEH = 64, F_PARA = 128, F_SWIM = 256, F_CLIMB = 512, F_COVER = 1024, F_DEAD = 2048 };

void Client::selfTest(Game& g, int me) {
  selfTested_ = true;
  Vec3 p = g.coords(me), v = g.velocity(me);
  logf("selftest: ped=%d model=%u pos=(%.1f,%.1f,%.1f) vel=(%.1f,%.1f,%.1f) heading=%.1f health=%d weapon=%u veh=%d", me, g.model(me), p.x, p.y, p.z, v.x, v.y, v.z, g.heading(me), g.health(me), g.weaponOf(me), g.vehicleOf(me));
}

void Client::tick(Natives& nat) {
  Game g(nat); double now = now_ms(); stats.ticks++;
  if (epochChanged_.exchange(false)) dropPuppets(g, true);
  int me = g.playerPed(); if (!me || !g.exists(me)) return;
  if (!selfTested_) selfTest(g, me);
  applyInbox(now);
  sampleLocal(g, me, now);
  if (!joined_) { if (!remotes_.empty() || !eps_.empty()) dropPuppets(g, false); world_.valid = false; applyWorld(g, now); applyPopulation(g, now); return; }
  applyWorld(g, now); applyPopulation(g, now); applyChat(g); applyWanted(g, now);
  // local one-shot animation requested through mp_say.txt (/anim dict clip [flags], /stopanim)
  { std::vector<std::string> la; { std::lock_guard<std::mutex> l(sh_.mu); la.swap(sh_.localAnim); }
    for (const std::string& s : la) { if (s == "stop") { g.V(nat::CLEAR_PED_TASKS, {A(me)}); pDict_.clear(); continue; }
      size_t a = s.find('|'), b = s.rfind('|'); if (a == std::string::npos || b == a) continue; pDict_ = s.substr(0, a); pClip_ = s.substr(a + 1, b - a - 1); pFlags_ = atoi(s.c_str() + b + 1); pSince_ = now; }
    if (!pDict_.empty()) { g.V(nat::REQUEST_ANIM_DICT, {P(pDict_.c_str())}); if (g.R(nat::HAS_ANIM_DICT_LOADED, {P(pDict_.c_str())}) & 1) { g.V(nat::TASK_PLAY_ANIM, {A(me), P(pDict_.c_str()), P(pClip_.c_str()), A(8.0f), A(-8.0f), A(-1), A(pFlags_), A(0.0f), A(false), A(false), A(false)}); pDict_.clear(); } else if (now - pSince_ > 4000) pDict_.clear(); } }
  updatePlayers(g, me, now);
  updateEntities(g, me, now);
  if (v2_) { if (!popOwn_) purgeAmbient(g, me, now); streamEntities(g, me, now); }
  stats.puppetPlayers = (int)std::count_if(remotes_.begin(), remotes_.end(), [](const std::pair<const int, Remote>& kv) { return kv.second.ped != 0; });
  stats.puppetEnts = (int)std::count_if(eps_.begin(), eps_.end(), [](const std::pair<const int, EPup>& kv) { return kv.second.handle != 0; });
}

// ------------------------------------------------------------------ inbox: net thread -> puppets tables
void Client::applyInbox(double now) {
  std::vector<PState> players; bool pNew = false; std::map<int, App> apps; std::map<int, Loadout> loads; std::vector<AnimEv> anims; std::vector<ERow> ents; std::vector<int> edel, left; std::vector<std::pair<int, int>> owns; std::vector<int> take;
  bool wNew = false; int wsec = 0; float wsc = 30; bool wfz = false; std::string ww; double wrecv = 0;
  { std::lock_guard<std::mutex> l(sh_.mu);
    players.swap(sh_.players); pNew = sh_.playersNew; sh_.playersNew = false; apps.swap(sh_.apps); loads.swap(sh_.loads); anims.swap(sh_.anims); ents.swap(sh_.ents); edel.swap(sh_.edel); left.swap(sh_.left); owns.swap(sh_.owns); take.swap(sh_.take);
    wNew = sh_.worldNew; sh_.worldNew = false; wsec = sh_.wsec; wsc = sh_.wscale; wfz = sh_.wfrozen; ww = sh_.wweather; wrecv = sh_.wrecv; }
  if (wNew) { world_.valid = true; world_.sec = wsec; world_.scale = wsc; world_.frozen = wfz; world_.weather = ww; world_.recv = wrecv; }
  if (pNew) for (PState& p : players) { if (p.id == myId_) continue; Remote& r = remotes_[p.id]; r.st = p; r.lastSeen = now; r.seen = true; }
  auto touch = [&](int id) -> Remote& { Remote& r = remotes_[id]; if (r.lastSeen == 0) r.lastSeen = now; return r; };   // never let a fresh entry look stale
  for (auto& kv : apps) { Remote& r = touch(kv.first); r.app = kv.second; r.appliedApp = false; }
  for (auto& kv : loads) touch(kv.first).load = kv.second;
  for (AnimEv& a : anims) { Remote& r = touch(a.id); if (a.start) { r.animDict = a.dict; r.animClip = a.clip; r.animFlags = a.flags; r.animPending = true; r.animSince = now; } else { r.animStop = true; r.animPending = false; } }
  for (int id : left) { auto it = remotes_.find(id); if (it != remotes_.end()) it->second.lastSeen = -1e9; }
  for (int g : take) killLids_.push_back(g & 0xFFFF);
  for (auto it = xferred_.begin(); it != xferred_.end();) it = (now - it->second > 15000) ? xferred_.erase(it) : std::next(it);
  for (ERow& e : ents) { if (e.ctl == myId_ || xferred_.count(e.g)) continue; EPup& p = eps_[e.g]; p.row = e; p.lastSeen = now; if (!e.d.empty()) { p.d = e.d; p.dNew = true; } }
  for (int gid : edel) { auto it = eps_.find(gid); if (it != eps_.end()) it->second.lastSeen = -1e9; }
  for (auto& o : owns) { auto it = eps_.find(o.first); if (it != eps_.end()) { if (o.second == myId_) it->second.lastSeen = -1e9; else it->second.row.ctl = o.second; } }
}

// ------------------------------------------------------------------ local player sampling -> net thread
void Client::sampleLocal(Game& g, int ped, double now) {
  if (now - lastSample_ < 30) return; lastSample_ = now;
  PState s; Vec3 p = g.coords(ped); s.x = p.x; s.y = p.y; s.z = p.z; s.h = g.heading(ped); s.hp = g.health(ped);
  int veh = g.vehicleOf(ped); Vec3 v;
  if (veh && g.exists(veh)) { v = g.velocity(veh); s.vm = std::to_string(g.model(veh)); localSd_ = g.seatOf(ped, veh); } else { veh = 0; v = g.velocity(ped); localSd_ = -2; }
  s.vx = v.x; s.vy = v.y; s.vz = v.z; s.wp = g.weaponOf(ped); s.am = localAm_;
  { Vec3 cr = g.vec(nat::GET_GAMEPLAY_CAM_ROT, {A(2)}); s.cp = cr.x; s.cy = cr.z; }
  if (veh) { if (now - lastVdAt_ >= 100) { lastVdAt_ = now; localVd_ = vehDetail(g, veh, now); } s.vdj = localVd_; } else localVd_.clear();   // where the player looks/aims (pitch, yaw)
  if (now - lastFlags_ >= 100) {
    lastFlags_ = now; int fl = 0;
    int pidx = (int)g.R(nat::GET_PLAYER_INDEX);
    if (g.R(nat::IS_PLAYER_FREE_AIMING, {A(pidx)}) & 1) fl |= F_AIM;
    if (g.flag(nat::IS_PED_SHOOTING, ped)) fl |= F_SHOOT;
    if (g.flag(nat::IS_PED_RELOADING, ped)) fl |= F_RELOAD;
    if (g.flag(nat::IS_PED_DUCKING, ped)) fl |= F_CROUCH;
    if (g.flag(nat::IS_PED_RAGDOLL, ped)) fl |= F_RAGDOLL;
    if (g.flag(nat::IS_PED_SPRINTING, ped)) fl |= F_SPRINT;
    if (veh) fl |= F_VEH;
    if (g.flag(nat::IS_PED_SWIMMING, ped)) fl |= F_SWIM;
    if (g.flag(nat::IS_PED_CLIMBING, ped)) fl |= F_CLIMB;
    if (g.R(nat::IS_PED_IN_COVER, {A(ped), A(false)}) & 1) fl |= F_COVER;
    if (g.R(nat::IS_PED_DEAD_OR_DYING, {A(ped), A(true)}) & 1) fl |= F_DEAD;
    localFl_ = fl; localAm_ = g.ammoOf(ped, s.wp);
  }
  s.fl = localFl_; s.sd = localSd_; s.am = localAm_;
  { std::lock_guard<std::mutex> l(sh_.mu); sh_.local = s; sh_.localValid = true; sh_.localAt = now; }
  if (!v2_) return;
  if (now - lastApp_ >= 500) {
    lastApp_ = now; std::string j = fmt("{\"t\":\"a\",\"m\":%u,\"c\":[", g.model(ped));
    for (int c = 0; c < 12; c++) j += fmt("%s%d,%d,%d", c ? "," : "", (int)g.R(nat::GET_PED_DRAWABLE_VARIATION, {A(ped), A(c)}), (int)g.R(nat::GET_PED_TEXTURE_VARIATION, {A(ped), A(c)}), (int)g.R(nat::GET_PED_PALETTE_VARIATION, {A(ped), A(c)}));
    j += "],\"p\":[";
    for (int i = 0; i < 8; i++) j += fmt("%s%d,%d", i ? "," : "", (int)g.R(nat::GET_PED_PROP_INDEX, {A(ped), A(i)}), (int)g.R(nat::GET_PED_PROP_TEXTURE_INDEX, {A(ped), A(i)}));
    j += "]}";
    if (j != lastAppJson_ || now - lastAppSent_ > 10000) { lastAppJson_ = j; lastAppSent_ = now; queueOut(j); }
  }
  if (s.wp != lastSentW_ || (s.am != lastSentAm_ && now - lastLoadSent_ > 1000)) { lastSentW_ = s.wp; lastSentAm_ = s.am; lastLoadSent_ = now; queueOut(fmt("{\"t\":\"wp\",\"w\":%u,\"am\":%d,\"c\":[]}", s.wp, s.am)); }
}

// ------------------------------------------------------------------ shared world: clock, weather, population
void Client::applyWorld(Game& g, double now) {
  if (!world_.valid) { if (clockPaused_) { g.V(nat::PAUSE_CLOCK, {A(false)}); clockPaused_ = false; appliedWeather_.clear(); g.V(nat::CLEAR_OVERRIDE_WEATHER); } return; }
  if (!clockPaused_) { g.V(nat::PAUSE_CLOCK, {A(true)}); clockPaused_ = true; }
  if (now - lastClock_ >= 50) {
    lastClock_ = now; double sec = world_.sec; if (!world_.frozen) sec += (now - world_.recv) / 1000.0 * world_.scale; sec = std::fmod(sec, 86400.0); if (sec < 0) sec += 86400.0;
    int s = (int)sec; g.V(nat::SET_CLOCK_TIME, {A(s / 3600), A((s / 60) % 60), A(s % 60)});
  }
  if (world_.weather != appliedWeather_) {
    static char wbuf[40]; snprintf(wbuf, sizeof wbuf, "%s", world_.weather.c_str());
    g.V(nat::CLEAR_OVERRIDE_WEATHER); g.V(nat::SET_WEATHER_TYPE_NOW_PERSIST, {P(wbuf)}); g.V(nat::SET_OVERRIDE_WEATHER, {P(wbuf)}); appliedWeather_ = world_.weather;
    logf("world: weather %s", wbuf);
  }
}
void Client::applyPopulation(Game& g, double now) {
  (void)now;
  bool want = joined_ && v2_ && !popOwn_;
  if (want) {
    if (!popSuppressed_) { g.V(nat::SET_PED_POPULATION_BUDGET, {A(0)}); g.V(nat::SET_VEHICLE_POPULATION_BUDGET, {A(0)}); popSuppressed_ = true; logf("population: local traffic off (mirroring the area owner)"); }
    g.V(nat::SET_PED_DENSITY_MULTIPLIER_THIS_FRAME, {A(0.0f)}); g.V(nat::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, {A(0.0f)}); g.V(nat::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, {A(0.0f)});
    g.V(nat::SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, {A(0.0f)}); g.V(nat::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME, {A(0.0f), A(0.0f)});
  } else if (popSuppressed_) { g.V(nat::SET_PED_POPULATION_BUDGET, {A(3)}); g.V(nat::SET_VEHICLE_POPULATION_BUDGET, {A(3)}); popSuppressed_ = false; logf("population: local traffic restored"); }
}

// ------------------------------------------------------------------ puppets
void Client::moveActor(Game& g, int ent, const Vec3& target, const Vec3& vel, float heading, double now, double& lastTask, bool& moving, bool aiming) {
  Vec3 cur = g.coords(ent); float d = dist3d(cur, target), spd = std::hypot(vel.x, vel.y);
  if (d > 6.0f) { g.setCoords(ent, target); g.setHeading(ent, heading); lastTask = 0; moving = false; return; }
  if (spd > 0.6f && !aiming) {
    if (now - lastTask > 200) {
      float sp = spd < 2.2f ? 1.0f : (spd < 5.5f ? 2.0f : 3.0f); Vec3 t = {target.x + vel.x * 0.2f, target.y + vel.y * 0.2f, target.z};
      g.V(nat::TASK_GO_STRAIGHT_TO_COORD, {A(ent), A(t.x), A(t.y), A(t.z), A(sp), A(-1), A(heading), A(0.5f)}); lastTask = now;
    }
    moving = true;
  } else {
    if (moving) { g.V(nat::CLEAR_PED_TASKS, {A(ent)}); moving = false; }
    if (d > 0.5f) g.setCoords(ent, lerp(cur, target, 0.5f));
    g.setHeading(ent, heading);
  }
}
void Client::applyAppearance(Game& g, Remote& r) {
  if (!r.ped || !r.app.valid) return;
  if (r.app.c.size() >= 36) for (int c = 0; c < 12; c++) { int d = r.app.c[c * 3], t = r.app.c[c * 3 + 1], p = r.app.c[c * 3 + 2]; if (d >= 0) g.V(nat::SET_PED_COMPONENT_VARIATION, {A(r.ped), A(c), A(d), A(std::max(t, 0)), A(std::max(p, 0))}); }
  for (int i = 0; i < 8 && (size_t)(2 * i + 1) < r.app.p.size(); i++) { int idx = r.app.p[2 * i], t = r.app.p[2 * i + 1]; if (idx >= 0) g.V(nat::SET_PED_PROP_INDEX, {A(r.ped), A(i), A(idx), A(std::max(t, 0)), A(true)}); else g.V(nat::CLEAR_PED_PROP, {A(r.ped), A(i)}); }
  r.appliedApp = true;
}
void Client::applyWeapon(Game& g, Remote& r) {
  if (!r.ped) return; uint32_t w = r.st.wp; if (w == r.curWeapon) return;
  if (w == 0 || w == kUnarmed) { g.V(nat::REMOVE_ALL_PED_WEAPONS, {A(r.ped), A(true)}); g.V(nat::SET_CURRENT_PED_WEAPON, {A(r.ped), A(kUnarmed), A(true)}); }
  else { g.V(nat::GIVE_WEAPON_TO_PED, {A(r.ped), A(w), A(9999), A(false), A(true)}); g.V(nat::SET_CURRENT_PED_WEAPON, {A(r.ped), A(w), A(true)}); }
  r.curWeapon = w;
}

void Client::updatePlayers(Game& g, int me, double now) {
  (void)me; int spawned = 0; const uint32_t defModel = joaat("a_m_y_hipster_01");
  for (auto it = remotes_.begin(); it != remotes_.end();) {
    Remote& r = it->second; int id = it->first;
    if (now - r.lastSeen > 3000) { if (r.ped) { ownedByUs_.erase(r.ped); g.del(r.ped); } if (r.veh) { ownedByUs_.erase(r.veh); g.del(r.veh); } logf("puppet for #%d removed", id); it = remotes_.erase(it); continue; }
    ++it; if (!r.seen) continue;
    Vec3 pos = {r.st.x, r.st.y, r.st.z}; bool inVeh = (r.st.fl & F_VEH) && !r.st.vm.empty(); uint32_t vm = inVeh ? parseHash(r.st.vm) : 0;
    // appearance model change => respawn
    if (r.ped && r.app.valid && r.app.model && r.app.model != r.pedModel) { ownedByUs_.erase(r.ped); g.del(r.ped); r.ped = 0; }
    if (!r.ped) {
      if (now < r.lastCoordSet || spawned >= 2) continue;
      uint32_t m = r.app.valid && r.app.model ? r.app.model : defModel; if (!g.modelValid(m)) m = defModel;
      if (r.spawnAt == 0) r.spawnAt = now; if (now - r.spawnAt > 8000 && m != defModel) m = defModel;
      if (!g.loadModel(m)) { if (now - r.spawnAt > 15000) { logf("spawn for #%d produced no entity, giving up", id); r.lastCoordSet = now + 10000; r.spawnAt = 0; } continue; }
      r.ped = g.createPed(m, pos, r.st.h); r.spawnAt = 0; spawned++;
      if (!r.ped) { logf("spawn for #%d produced no entity, giving up", id); r.lastCoordSet = now + 10000; g.releaseModel(m); continue; }
      r.pedModel = m; ownedByUs_.insert(r.ped); r.curWeapon = 0xFFFFFFFFu; r.moving = false; r.appliedApp = false;
      g.V(nat::SET_ENTITY_AS_MISSION_ENTITY, {A(r.ped), A(true), A(true)}); g.V(nat::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS, {A(r.ped), A(true)}); g.V(nat::SET_ENTITY_INVINCIBLE, {A(r.ped), A(true)});
      g.V(nat::SET_PED_CAN_RAGDOLL, {A(r.ped), A(false)}); g.V(nat::SET_PED_KEEP_TASK, {A(r.ped), A(true)}); g.releaseModel(m);
      logf("spawned ped %d for #%d (%s)", r.ped, id, r.st.name.c_str());
    }
    if (!g.exists(r.ped)) { ownedByUs_.erase(r.ped); r.ped = 0; continue; }
    if (!r.appliedApp) applyAppearance(g, r);
    applyWeapon(g, r);
    if (r.animStop) { g.V(nat::CLEAR_PED_TASKS, {A(r.ped)}); r.animStop = false; }
    if (r.animPending) { g.V(nat::REQUEST_ANIM_DICT, {P(r.animDict.c_str())}); if (g.R(nat::HAS_ANIM_DICT_LOADED, {P(r.animDict.c_str())}) & 1) { g.V(nat::TASK_PLAY_ANIM, {A(r.ped), P(r.animDict.c_str()), P(r.animClip.c_str()), A(8.0f), A(-8.0f), A(-1), A(r.animFlags), A(0.0f), A(false), A(false), A(false)}); r.animPending = false; } else if (now - r.animSince > 4000) r.animPending = false; }
    // leaving a car: wait 700 ms so a single flickering packet does not delete and respawn the car
    if (!inVeh && r.veh) { if (r.vehOutSince == 0) r.vehOutSince = now; if (now - r.vehOutSince < 700) { r.lastFl = r.st.fl; continue; } } else r.vehOutSince = 0;
    // vehicle puppet
    if (inVeh && vm && g.modelValid(vm)) {
      if (r.veh && (!g.exists(r.veh) || r.vehModel != vm)) { logf("puppet car of #%d replaced (%s)", id, g.exists(r.veh) ? "model changed" : "handle vanished"); ownedByUs_.erase(r.veh); g.del(r.veh); r.veh = 0; }
      if (!r.veh) {
        if (g.loadModel(vm)) { r.veh = g.createVehicle(vm, pos, r.st.h); if (r.veh) { r.vehModel = vm; ownedByUs_.insert(r.veh); g.V(nat::SET_ENTITY_AS_MISSION_ENTITY, {A(r.veh), A(true), A(true)}); proofOn(g, r.veh, true); g.V(nat::SET_PED_INTO_VEHICLE, {A(r.ped), A(r.veh), A(r.st.sd < -1 ? -1 : r.st.sd)}); logf("spawned vehicle %d for #%d", r.veh, id); } g.releaseModel(vm); }
      } else {
        Vec3 cur = g.coords(r.veh), vel = {r.st.vx, r.st.vy, r.st.vz}; float d = dist3d(cur, pos);
        if (d > 8.0f) { g.setCoords(r.veh, pos); g.setHeading(r.veh, r.st.h); g.setVelocity(r.veh, vel); }
        else { Vec3 fix = {vel.x + (pos.x - cur.x) * 3.0f, vel.y + (pos.y - cur.y) * 3.0f, vel.z + (pos.z - cur.z) * 3.0f}; g.setVelocity(r.veh, fix); if (r.st.vd.size() < 8 && now - r.lastTask > 100) { g.setHeading(r.veh, r.st.h); r.lastTask = now; } }
        if (r.st.vd.size() >= 8 && r.st.vd != r.vdApplied) { applyVehDetail(g, r.veh, r.st.vd, r.vbits); r.vdApplied = r.st.vd; }
        if (g.vehicleOf(r.ped) != r.veh) g.V(nat::SET_PED_INTO_VEHICLE, {A(r.ped), A(r.veh), A(r.st.sd < -1 ? -1 : r.st.sd)});
      }
      continue;
    }
    if (r.veh) { // exit: get the ped out FIRST (warp out, detach, restore collision/visibility), only then delete the car
      g.V(nat::TASK_LEAVE_VEHICLE, {A(r.ped), A(r.veh), A(16)}); g.V(nat::CLEAR_PED_TASKS_IMMEDIATELY, {A(r.ped)});
      g.V(nat::DETACH_ENTITY, {A(r.ped), A(true), A(true)}); g.setCoords(r.ped, pos);
      g.V(nat::SET_ENTITY_COLLISION, {A(r.ped), A(true), A(false)}); g.V(nat::SET_ENTITY_VISIBLE, {A(r.ped), A(true), A(false)});
      ownedByUs_.erase(r.veh); g.del(r.veh); r.veh = 0; r.moving = false; r.lastTask = 0; logf("puppet of #%d left its car", id);
    }
    // on foot
    if ((r.st.fl & F_DEAD) && !r.dead) { g.V(nat::SET_ENTITY_INVINCIBLE, {A(r.ped), A(false)}); g.V(nat::SET_PED_CAN_RAGDOLL, {A(r.ped), A(true)}); g.V(nat::SET_ENTITY_HEALTH, {A(r.ped), A(0)}); r.dead = true; }
    if (!(r.st.fl & F_DEAD) && r.dead) { g.V(nat::CLEAR_PED_TASKS_IMMEDIATELY, {A(r.ped)}); g.V(nat::SET_ENTITY_HEALTH, {A(r.ped), A(200)}); g.V(nat::SET_ENTITY_INVINCIBLE, {A(r.ped), A(true)}); g.V(nat::SET_PED_CAN_RAGDOLL, {A(r.ped), A(false)}); g.setCoords(r.ped, pos); r.dead = false; }
    if (r.dead) continue;
    bool crouch = (r.st.fl & F_CROUCH) != 0; if (crouch != r.crouch) { g.V(nat::SET_PED_STEALTH_MOVEMENT, {A(r.ped), A(crouch), A(0)}); r.crouch = crouch; }
    if ((r.st.fl & F_RAGDOLL) && !(r.lastFl & F_RAGDOLL)) g.V(nat::SET_PED_TO_RAGDOLL, {A(r.ped), A(1500), A(1500), A(0), A(false), A(false), A(false)});
    bool aim = (r.st.fl & (F_AIM | F_SHOOT)) != 0 && r.st.wp != 0 && r.st.wp != kUnarmed;
    Vec3 vel = {r.st.vx, r.st.vy, r.st.vz}; Vec3 target = pos;
    if (!(r.st.fl & F_RAGDOLL)) moveActor(g, r.ped, target, vel, r.st.h, now, r.lastTask, r.moving, aim);
    if (aim) {
      Vec3 at; if (r.st.cp != 0.0f || r.st.cy != 0.0f) { float yr = r.st.cy * kPi / 180.0f, pr = r.st.cp * kPi / 180.0f; at = {pos.x - std::sin(yr) * std::cos(pr) * 60.0f, pos.y + std::cos(yr) * std::cos(pr) * 60.0f, pos.z + 0.6f + std::sin(pr) * 60.0f}; }
      else { float hr = r.st.h * kPi / 180.0f; at = {pos.x - std::sin(hr) * 20.0f, pos.y + std::cos(hr) * 20.0f, pos.z + 0.4f}; }
      if (r.st.fl & F_SHOOT) g.V(nat::SET_PED_SHOOTS_AT_COORD, {A(r.ped), A(at.x), A(at.y), A(at.z), A(true)});
      else if (now - r.spawnAt > 300) g.V(nat::TASK_AIM_GUN_AT_COORD, {A(r.ped), A(at.x), A(at.y), A(at.z), A(600), A(false), A(false)});
    }
    r.lastFl = r.st.fl;
  }
}

bool Client::dupOfPlayerCar(const Vec3& pos, uint32_t model) const {
  for (const auto& kv : remotes_) { const Remote& r = kv.second; if (!r.seen || !(r.st.fl & F_VEH) || r.st.vm.empty() || parseHash(r.st.vm) != model) continue;
    if (dist3d({r.st.x, r.st.y, r.st.z}, pos) < 7.0f) return true; }
  return false;
}

// another player owns the ambient population around me: remove whatever my own game spawned (the owner streams the real ones)
void Client::purgeAmbient(Game& g, int me, double now) {
  if (now - lastPurge_ < 1000) return; lastPurge_ = now;
  int myVeh = g.vehicleOf(me), purged = 0;
  for (int kind = 1; kind <= 2; kind++) {
    int buf[2 * 40 + 2]; memset(buf, 0, sizeof buf); buf[0] = 40;
    int cnt = kind == 1 ? (int)g.R(nat::GET_PED_NEARBY_PEDS, {A(me), P(buf), A(-1)}) : (int)g.R(nat::GET_PED_NEARBY_VEHICLES, {A(me), P(buf)});
    if (cnt < 0) cnt = 0; if (cnt > 40) cnt = 40;
    for (int i = 1; i <= 2 * cnt + 1; i++) {
      int h = buf[i]; if (h <= 0 || h == me || h == myVeh || ownedByUs_.count(h) || taken_.count(h) || !g.exists(h)) continue;
      if ((int)g.R(nat::GET_ENTITY_TYPE, {A(h)}) != kind) continue;
      if (kind == 1 && (g.flag(nat::IS_PED_A_PLAYER, h) || (myVeh && g.vehicleOf(h) == myVeh))) continue;
      int pt = (int)g.R(nat::GET_ENTITY_POPULATION_TYPE, {A(h)});
      if (pt >= 2 && pt <= 5) { g.del(h); purged++; }   // random parked/patrol/scenario/ambient only: never mission or player-made entities
    }
  }
  if (purged) logf("purged %d local ambient entities (area owned by another player)", purged);
}

// full state of a car as 8 numbers: quaternion(4), steering(reserved), bits, body health, engine health
std::string Client::vehDetail(Game& g, int h, double now) {
  float q[4] = {0, 0, 0, 1}; g.V(nat::GET_ENTITY_QUATERNION, {A(h), P(&q[0]), P(&q[1]), P(&q[2]), P(&q[3])});
  if (!g.lastOk()) return "";
  VdC& c = vdc_[h];
  if (now - c.at > 500) { int s = 0;   // doors and tyres change rarely: poll them twice a second
    for (int i = 0; i < 8; i++) if (F(g.R(nat::GET_VEHICLE_DOOR_ANGLE_RATIO, {A(h), A(i)})) > 0.1f) s |= 1 << i;
    for (int i = 0; i < 6; i++) if (g.R(nat::IS_VEHICLE_TYRE_BURST, {A(h), A(i), A(false)}) & 1) s |= 1 << (8 + i);
    c.slow = s; c.at = now; }
  int on = 0, hb = 0; g.V(nat::GET_VEHICLE_LIGHTS_STATE, {A(h), P(&on), P(&hb)});
  int bits = (int)(g.R(nat::GET_IS_VEHICLE_ENGINE_RUNNING, {A(h)}) & 1) | ((on & 1) ? 2 : 0) | ((hb & 1) ? 4 : 0) | ((g.R(nat::IS_VEHICLE_SIREN_ON, {A(h)}) & 1) ? 8 : 0) | ((g.R(nat::IS_HORN_ACTIVE, {A(h)}) & 1) ? 16 : 0) | ((c.slow & 0xFF) << 8) | (((c.slow >> 8) & 0x3F) << 16);
  int extra = ((g.R(nat::IS_ENTITY_DEAD, {A(h), A(false)}) & 1) ? 1 : 0) | ((g.R(nat::IS_ENTITY_ON_FIRE, {A(h)}) & 1) ? 2 : 0);   // exploded / burning
  return fmt("[%.4f,%.4f,%.4f,%.4f,%d,%d,%.0f,%.0f]", q[0], q[1], q[2], q[3], extra, bits, F(g.R(nat::GET_VEHICLE_BODY_HEALTH, {A(h)})), F(g.R(nat::GET_VEHICLE_ENGINE_HEALTH, {A(h)})));
}

void Client::applyVehDetail(Game& g, int veh, const std::vector<float>& d, int& lastBits) {
  if (d.size() < 8) return;
  g.V(nat::SET_ENTITY_QUATERNION, {A(veh), A(d[0]), A(d[1]), A(d[2]), A(d[3])});
  int bits = (int)d[5] | (((int)d[4]) << 26), prev = lastBits < 0 ? ~bits : lastBits, diff = bits ^ prev;
  if ((bits & (1 << 26)) && !(prev & (1 << 26))) { proofOn(g, veh, false); g.V(nat::EXPLODE_VEHICLE, {A(veh), A(true), A(false)}); }   // the owner's car blew up
  if (diff & (1 << 27)) { if (bits & (1 << 27)) g.V(nat::START_ENTITY_FIRE, {A(veh)}); else g.V(nat::STOP_ENTITY_FIRE, {A(veh)}); }
  if (diff & 1) g.V(nat::SET_VEHICLE_ENGINE_ON, {A(veh), A((bits & 1) != 0), A(true), A(true)});
  if (diff & 6) { g.V(nat::SET_VEHICLE_LIGHTS, {A(veh), A((bits & 2) ? 2 : 1)}); g.V(nat::SET_VEHICLE_FULLBEAM, {A(veh), A((bits & 4) != 0)}); }
  if (diff & 8) g.V(nat::SET_VEHICLE_SIREN, {A(veh), A((bits & 8) != 0)});
  if ((bits & 16) && !(prev & 16)) g.V(nat::START_VEHICLE_HORN, {A(veh), A(400), A(joaat("HELDDOWN")), A(false)});
  for (int i = 0; i < 8; i++) if (diff & (1 << (8 + i))) { if (bits & (1 << (8 + i))) g.V(nat::SET_VEHICLE_DOOR_OPEN, {A(veh), A(i), A(false), A(false)}); else g.V(nat::SET_VEHICLE_DOOR_SHUT, {A(veh), A(i), A(true)}); }
  for (int i = 0; i < 6; i++) if ((bits & (1 << (16 + i))) && !(prev & (1 << (16 + i)))) g.V(nat::SET_VEHICLE_TYRE_BURST, {A(veh), A(i), A(true), A(1000.0f)});
  if (d[6] > 0 && std::fabs(F(g.R(nat::GET_VEHICLE_BODY_HEALTH, {A(veh)})) - d[6]) > 50.0f) g.V(nat::SET_VEHICLE_BODY_HEALTH, {A(veh), A(d[6])});
  if (d[7] > 0 && std::fabs(F(g.R(nat::GET_VEHICLE_ENGINE_HEALTH, {A(veh)})) - d[7]) > 50.0f) g.V(nat::SET_VEHICLE_ENGINE_HEALTH, {A(veh), A(d[7])});
  lastBits = bits;
}

void Client::applyChat(Game& g) {
  std::vector<std::string> c; { std::lock_guard<std::mutex> l(sh_.mu); c.swap(sh_.chatIn); }
  static std::string buf;
  for (const std::string& line : c) for (size_t i = 0; i < line.size(); i += 90) {
    buf = line.substr(i, 90);   // the ticker shows one text component of up to ~99 chars
    g.V(nat::BEGIN_TEXT_COMMAND_THEFEED_POST, {P("STRING")}); g.V(nat::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME, {P(buf.c_str())}); g.V(nat::END_TEXT_COMMAND_THEFEED_POST_TICKER, {A(false), A(false)});
  }
}

// wanted level is shared: the last player whose level changed sets it for everyone
void Client::applyWanted(Game& g, double now) {
  int pidx = (int)g.R(nat::GET_PLAYER_INDEX);
  std::vector<int> in; { std::lock_guard<std::mutex> l(sh_.mu); in.swap(sh_.wlIn); }
  for (int l : in) { appliedWl_ = l; wlSetAt_ = now; g.V(nat::SET_PLAYER_WANTED_LEVEL, {A(pidx), A(l), A(false)}); g.V(nat::SET_PLAYER_WANTED_LEVEL_NOW, {A(pidx), A(false)}); logf("wanted level set to %d by another player", l); }
  if (now - lastWlPoll_ < 250 || now - wlSetAt_ < 1500) return; lastWlPoll_ = now;
  int lv = (int)g.R(nat::GET_PLAYER_WANTED_LEVEL, {A(pidx)});
  if (lv != appliedWl_) { appliedWl_ = lv; queueOut("{\"t\":\"wl\",\"l\":" + std::to_string(lv) + "}"); logf("my wanted level is %d", lv); }
}

void Client::updateEntities(Game& g, int me, double now) {
  int spawned = 0;
  { int mv = g.vehicleOf(me); // I climbed into a mirrored NPC car: I own it from now on
    if (mv && g.exists(mv)) for (auto it = eps_.begin(); it != eps_.end(); ++it) if (it->second.handle == mv && it->second.row.k == 2) {
      int gid = it->first; queueOut("{\"t\":\"xfer\",\"g\":" + std::to_string(gid) + "}"); xferred_[gid] = now; ownedByUs_.erase(mv); taken_.insert(mv); proofOn(g, mv, false);
      logf("took over car %d (gid %d)", mv, gid); eps_.erase(it); break; } }
  for (auto it = eps_.begin(); it != eps_.end();) {
    EPup& e = it->second;
    if (now - e.lastSeen > 2200) { if (e.handle) { ownedByUs_.erase(e.handle); g.del(e.handle); } it = eps_.erase(it); continue; }
    ++it; const ERow& r = e.row; Vec3 pos = {r.x, r.y, r.z}, vel = {r.vx, r.vy, r.vz};
    // never show the same car twice: an NPC car that is where a player's car puppet is, is that player's car (and so is its driver)
    if ((r.k == 2 && dupOfPlayerCar(pos, r.m)) || (r.k == 1 && (r.fl & F_VEH) && xferred_.count(r.b))) {
      int gid = r.g; if (e.handle) { ownedByUs_.erase(e.handle); g.del(e.handle); } xferred_[gid] = now; logf("dedupe: removed duplicate %s %d", r.k == 2 ? "car" : "ped", gid); eps_.erase(gid); continue; }
    if (!e.handle) {
      if (spawned >= 2 || !g.modelValid(r.m)) continue;
      if (e.spawnAt == 0) e.spawnAt = now;
      if (!g.loadModel(r.m)) { if (now - e.spawnAt > 10000) { e.lastSeen = -1e9; } continue; }
      e.handle = r.k == 2 ? g.createVehicle(r.m, pos, r.h) : g.createPed(r.m, pos, r.h); spawned++;
      if (!e.handle) { e.lastSeen = -1e9; g.releaseModel(r.m); continue; }
      ownedByUs_.insert(e.handle); g.V(nat::SET_ENTITY_AS_MISSION_ENTITY, {A(e.handle), A(true), A(true)});
      if (r.k == 1) { g.V(nat::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS, {A(e.handle), A(true)}); g.V(nat::SET_ENTITY_INVINCIBLE, {A(e.handle), A(true)}); g.V(nat::SET_PED_CAN_RAGDOLL, {A(e.handle), A(false)}); g.V(nat::SET_PED_RANDOM_COMPONENT_VARIATION, {A(e.handle), A(0)}); g.V(nat::SET_PED_KEEP_TASK, {A(e.handle), A(true)}); }
      else { int p = r.a & 0xFF, s = (r.a >> 8) & 0xFF; g.V(nat::SET_VEHICLE_COLOURS, {A(e.handle), A(p), A(s)}); proofOn(g, e.handle, true); }
      g.releaseModel(r.m); continue;
    }
    if (!g.exists(e.handle)) { ownedByUs_.erase(e.handle); e.handle = 0; e.spawnAt = 0; continue; }
    if (r.k == 2) {
      Vec3 cur = g.coords(e.handle); float d = dist3d(cur, pos);
      if (d > 8.0f) { g.setCoords(e.handle, pos); g.setHeading(e.handle, r.h); g.setVelocity(e.handle, vel); }
      else { g.setVelocity(e.handle, {vel.x + (pos.x - cur.x) * 3.0f, vel.y + (pos.y - cur.y) * 3.0f, vel.z + (pos.z - cur.z) * 3.0f}); if (e.d.size() < 8 && now - e.lastTask > 100) { g.setHeading(e.handle, r.h); e.lastTask = now; } }
      if (e.dNew && e.d.size() >= 8) { applyVehDetail(g, e.handle, e.d, e.vbits); e.dNew = false; }
    } else {
      if (e.dNew && e.d.size() >= 24) { for (int c = 0; c < 12; c++) g.V(nat::SET_PED_COMPONENT_VARIATION, {A(e.handle), A(c), A((int)e.d[2 * c]), A(std::max((int)e.d[2 * c + 1], 0)), A(0)}); e.dNew = false; }
      { bool dead = (r.fl & F_DEAD) != 0;
        if (dead && !e.dead) { g.V(nat::SET_ENTITY_INVINCIBLE, {A(e.handle), A(false)}); g.V(nat::SET_PED_CAN_RAGDOLL, {A(e.handle), A(true)}); g.V(nat::SET_ENTITY_HEALTH, {A(e.handle), A(0)}); e.dead = true; }
        if (e.dead) { if (!dead) { g.V(nat::SET_ENTITY_INVINCIBLE, {A(e.handle), A(true)}); g.V(nat::SET_PED_CAN_RAGDOLL, {A(e.handle), A(false)}); g.V(nat::CLEAR_PED_TASKS_IMMEDIATELY, {A(e.handle)}); g.V(nat::SET_ENTITY_HEALTH, {A(e.handle), A(200)}); g.setCoords(e.handle, pos); e.dead = false; } else continue; } }
      bool inVeh = (r.fl & F_VEH) && r.b != 0; auto vi = inVeh ? eps_.find(r.b) : eps_.end();
      if (inVeh && vi != eps_.end() && vi->second.handle) { if (!e.seated) { g.V(nat::SET_PED_INTO_VEHICLE, {A(e.handle), A(vi->second.handle), A(((r.fl >> 12) & 0xF) - 2)}); e.seated = true; } continue; }
      if (e.seated) { g.V(nat::CLEAR_PED_TASKS_IMMEDIATELY, {A(e.handle)}); g.setCoords(e.handle, pos); e.seated = false; }
      if (inVeh) continue;
      { uint32_t w = (uint32_t)r.b;   // on foot: b = weapon in hand
        if (w && w != e.wep) { g.V(nat::GIVE_WEAPON_TO_PED, {A(e.handle), A(w), A(999), A(false), A(true)}); g.V(nat::SET_CURRENT_PED_WEAPON, {A(e.handle), A(w), A(true)}); e.wep = w; }
        else if (!w && e.wep && e.wep != 0xFFFFFFFFu) { g.V(nat::REMOVE_ALL_PED_WEAPONS, {A(e.handle), A(true)}); e.wep = 0; }
        if ((r.fl & F_RAGDOLL) && !(e.lastFl & F_RAGDOLL)) g.V(nat::SET_PED_TO_RAGDOLL, {A(e.handle), A(1500), A(1500), A(0), A(false), A(false), A(false)});
        bool shoot = w && (r.fl & F_SHOOT);
        if (!(r.fl & F_RAGDOLL)) moveActor(g, e.handle, pos, vel, r.h, now, e.lastTask, e.moving, shoot);
        if (shoot) { float hr = r.h * kPi / 180.0f; g.V(nat::SET_PED_SHOOTS_AT_COORD, {A(e.handle), A(pos.x - std::sin(hr) * 20.0f), A(pos.y + std::cos(hr) * 20.0f), A(pos.z + 0.4f), A(true)}); }
        e.lastFl = r.fl; }
    }
  }
}

// ------------------------------------------------------------------ host: stream the ambient world to everyone else
void Client::streamEntities(Game& g, int me, double now) {
  for (int lid : killLids_) for (auto it = lidOf_.begin(); it != lidOf_.end(); ++it) if (it->second == lid) {
    int h = it->first; int drv = g.exists(h) ? (int)g.R(nat::GET_PED_IN_VEHICLE_SEAT, {A(h), A(-1), A(0)}) : 0;
    if (drv && drv != me && g.exists(drv)) g.del(drv); if (h != g.vehicleOf(me) && g.exists(h)) g.del(h);
    logf("entity %d handed over to another player, removed locally", h); lidSeen_.erase(h); lidOf_.erase(it); break; }
  killLids_.clear();
  if (now - lastStream_ < 1000.0 / entRate_) return; lastStream_ = now;
  std::set<int> seen; std::vector<std::string> rows; int rawPeds = -1, rawVehs = -1, nPeds = 0, nVehs = 0;
  auto lid = [&](int h) { auto it = lidOf_.find(h); if (it == lidOf_.end()) { it = lidOf_.emplace(h, nextLid_).first; nextLid_ = nextLid_ % 65535 + 1; } lidSeen_[h] = now; return (myId_.load() << 16) | it->second; };
  int myVeh = g.vehicleOf(me);
  if (myVeh) { auto it = lidOf_.find(myVeh); if (it != lidOf_.end()) { queueOut("{\"t\":\"ed\",\"l\":[" + std::to_string((myId_.load() << 16) | it->second) + "]}"); lidSeen_.erase(myVeh); lidOf_.erase(it); } }
  auto emit = [&](int h, int kind) {
      (kind == 1 ? nPeds : nVehs)++; seen.insert(h); Vec3 p = g.coords(h), v = g.velocity(h); int gid = lid(h), fl = 0, a = 0, b = 0;
      std::string det;
      if (kind == 1) {
        a = g.health(h); int veh = g.vehicleOf(h);
        if (veh && g.exists(veh)) { fl |= F_VEH | (((g.seatOf(h, veh) + 2) & 0xF) << 12); b = lid(veh); }
        else { b = (int)g.weaponOf(h); if (g.flag(nat::IS_PED_SHOOTING, h)) fl |= F_SHOOT; }
        if (g.flag(nat::IS_PED_RAGDOLL, h)) fl |= F_RAGDOLL;
        if (g.R(nat::IS_PED_DEAD_OR_DYING, {A(h), A(true)}) & 1) fl |= F_DEAD;
        double& at = pedDetAt_[h];   // clothes: on first sight and every 3 s
        if (now - at > 3000) { at = now; det = "["; for (int c = 0; c < 12; c++) det += fmt("%s%d,%d", c ? "," : "", (int)g.R(nat::GET_PED_DRAWABLE_VARIATION, {A(h), A(c)}), (int)g.R(nat::GET_PED_TEXTURE_VARIATION, {A(h), A(c)})); det += "]"; }
      } else { int pc = 0, sc = 0; g.V(nat::GET_VEHICLE_COLOURS, {A(h), P(&pc), P(&sc)}); a = (pc & 0xFF) | ((sc & 0xFF) << 8); det = vehDetail(g, h, now); }
      rows.push_back(fmt("[%d,%d,%u,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%d,%d,%d%s%s]", gid, kind, g.model(h), p.x, p.y, p.z, g.heading(h), v.x, v.y, v.z, fl, a, b, det.empty() ? "" : ",", det.c_str()));
  };
  auto scan = [&](int kind) {
    int buf[2 * 40 + 2]; memset(buf, 0, sizeof buf); buf[0] = 40;
    int cnt = kind == 1 ? (int)g.R(nat::GET_PED_NEARBY_PEDS, {A(me), P(buf), A(-1)}) : (int)g.R(nat::GET_PED_NEARBY_VEHICLES, {A(me), P(buf)});
    (kind == 1 ? rawPeds : rawVehs) = cnt; if (!g.lastOk()) logf("stream: native for kind %d was rejected", kind);
    if (cnt < 0) cnt = 0; if (cnt > 40) cnt = 40;
    for (int i = 1; i <= 2 * cnt + 1; i++) {
      int h = buf[i]; if (h <= 0 || h == me || h == myVeh || ownedByUs_.count(h) || seen.count(h) || !g.exists(h)) continue;
      if ((int)g.R(nat::GET_ENTITY_TYPE, {A(h)}) != kind) continue;
      if (kind == 1 && g.flag(nat::IS_PED_A_PLAYER, h)) continue;
      emit(h, kind);
    }
  };
  if (popOwn_) { scan(1); scan(2); }
  for (auto it = taken_.begin(); it != taken_.end();) { int h = *it; if (!g.exists(h)) { it = taken_.erase(it); continue; } ++it; if (h == myVeh || seen.count(h)) continue; int k = (int)g.R(nat::GET_ENTITY_TYPE, {A(h)}); if (k == 1 || k == 2) emit(h, k); }
  if (now - lastStreamLog_ > 5000) { lastStreamLog_ = now; logf("stream: nearby peds %d (sent %d), vehicles %d (sent %d), rows %zu", rawPeds, nPeds, rawVehs, nVehs, rows.size()); }
  std::string cur = "{\"t\":\"e\",\"e\":["; int n = 0;
  auto flush = [&]() { if (n) { cur += "]}"; queueOut(cur); } cur = "{\"t\":\"e\",\"e\":["; n = 0; };
  for (const std::string& r : rows) { if (cur.size() + r.size() + 2 > 1250 || n >= 60) flush(); cur += (n ? "," : "") + r; n++; }
  flush();
  // entities that left the scan: tell everyone to remove them
  std::vector<int> gone;
  for (auto it = lidOf_.begin(); it != lidOf_.end();) { int h = it->first; if (!seen.count(h) && (now - lidSeen_[h] > 1500 || !g.exists(h))) { gone.push_back((myId_.load() << 16) | it->second); lidSeen_.erase(h); it = lidOf_.erase(it); } else ++it; }
  for (size_t i = 0; i < gone.size(); i += 40) { std::string s = "{\"t\":\"ed\",\"l\":["; for (size_t k = i; k < gone.size() && k < i + 40; k++) s += (k > i ? "," : "") + std::to_string(gone[k]); queueOut(s + "]}"); }
}

void Client::dropPuppets(Game& g, bool engineGone) {
  taken_.clear(); vdc_.clear(); pedDetAt_.clear();
  for (auto& kv : remotes_) { Remote& r = kv.second; if (!engineGone) { if (r.ped) g.del(r.ped); if (r.veh) g.del(r.veh); } r.ped = 0; r.veh = 0; r.curWeapon = 0xFFFFFFFFu; r.appliedApp = false; r.spawnAt = 0; r.moving = false; r.dead = false; r.crouch = false; }
  if (!engineGone) remotes_.clear();
  for (auto& kv : eps_) if (!engineGone && kv.second.handle) g.del(kv.second.handle);
  eps_.clear(); xferred_.clear(); ownedByUs_.clear(); lidOf_.clear(); lidSeen_.clear(); clockPaused_ = engineGone ? false : clockPaused_; appliedWeather_.clear();
  if (engineGone) popSuppressed_ = false;
}
}  // namespace mp
