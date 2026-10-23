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
  applyWorld(g, now); applyPopulation(g, now);
  // local one-shot animation requested through mp_say.txt (/anim dict clip [flags], /stopanim)
  { std::vector<std::string> la; { std::lock_guard<std::mutex> l(sh_.mu); la.swap(sh_.localAnim); }
    for (const std::string& s : la) { if (s == "stop") { g.V(nat::CLEAR_PED_TASKS, {A(me)}); pDict_.clear(); continue; }
      size_t a = s.find('|'), b = s.rfind('|'); if (a == std::string::npos || b == a) continue; pDict_ = s.substr(0, a); pClip_ = s.substr(a + 1, b - a - 1); pFlags_ = atoi(s.c_str() + b + 1); pSince_ = now; }
    if (!pDict_.empty()) { g.V(nat::REQUEST_ANIM_DICT, {P(pDict_.c_str())}); if (g.R(nat::HAS_ANIM_DICT_LOADED, {P(pDict_.c_str())}) & 1) { g.V(nat::TASK_PLAY_ANIM, {A(me), P(pDict_.c_str()), P(pClip_.c_str()), A(8.0f), A(-8.0f), A(-1), A(pFlags_), A(0.0f), A(false), A(false), A(false)}); pDict_.clear(); } else if (now - pSince_ > 4000) pDict_.clear(); } }
  updatePlayers(g, me, now);
  updateEntities(g, me, now);
  if (v2_ && hostId_ != 0 && hostId_ == myId_) streamEntities(g, me, now);
  stats.puppetPlayers = (int)std::count_if(remotes_.begin(), remotes_.end(), [](const std::pair<const int, Remote>& kv) { return kv.second.ped != 0; });
  stats.puppetEnts = (int)std::count_if(eps_.begin(), eps_.end(), [](const std::pair<const int, EPup>& kv) { return kv.second.handle != 0; });
}

// ------------------------------------------------------------------ inbox: net thread -> puppets tables
void Client::applyInbox(double now) {
  std::vector<PState> players; bool pNew = false; std::map<int, App> apps; std::map<int, Loadout> loads; std::vector<AnimEv> anims; std::vector<ERow> ents; std::vector<int> edel, left; std::vector<std::pair<int, int>> owns;
  bool wNew = false; int wsec = 0; float wsc = 30; bool wfz = false; std::string ww; double wrecv = 0;
  { std::lock_guard<std::mutex> l(sh_.mu);
    players.swap(sh_.players); pNew = sh_.playersNew; sh_.playersNew = false; apps.swap(sh_.apps); loads.swap(sh_.loads); anims.swap(sh_.anims); ents.swap(sh_.ents); edel.swap(sh_.edel); left.swap(sh_.left); owns.swap(sh_.owns);
    wNew = sh_.worldNew; sh_.worldNew = false; wsec = sh_.wsec; wsc = sh_.wscale; wfz = sh_.wfrozen; ww = sh_.wweather; wrecv = sh_.wrecv; }
  if (wNew) { world_.valid = true; world_.sec = wsec; world_.scale = wsc; world_.frozen = wfz; world_.weather = ww; world_.recv = wrecv; }
  if (pNew) for (PState& p : players) { if (p.id == myId_) continue; Remote& r = remotes_[p.id]; r.st = p; r.lastSeen = now; r.seen = true; }
  auto touch = [&](int id) -> Remote& { Remote& r = remotes_[id]; if (r.lastSeen == 0) r.lastSeen = now; return r; };   // never let a fresh entry look stale
  for (auto& kv : apps) { Remote& r = touch(kv.first); r.app = kv.second; r.appliedApp = false; }
  for (auto& kv : loads) touch(kv.first).load = kv.second;
  for (AnimEv& a : anims) { Remote& r = touch(a.id); if (a.start) { r.animDict = a.dict; r.animClip = a.clip; r.animFlags = a.flags; r.animPending = true; r.animSince = now; } else { r.animStop = true; r.animPending = false; } }
  for (int id : left) { auto it = remotes_.find(id); if (it != remotes_.end()) it->second.lastSeen = -1e9; }
  for (ERow& e : ents) { if (e.ctl == myId_) continue; EPup& p = eps_[e.g]; p.row = e; p.lastSeen = now; }
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
  bool want = joined_ && v2_ && pop_ == "host" && hostId_ != 0 && hostId_ != myId_;
  if (want) {
    if (!popSuppressed_) { g.V(nat::SET_PED_POPULATION_BUDGET, {A(0)}); g.V(nat::SET_VEHICLE_POPULATION_BUDGET, {A(0)}); popSuppressed_ = true; logf("population: mirroring host #%d (local traffic off)", hostId_.load()); }
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
    // vehicle puppet
    if (inVeh && vm && g.modelValid(vm)) {
      if (r.veh && (!g.exists(r.veh) || r.vehModel != vm)) { ownedByUs_.erase(r.veh); g.del(r.veh); r.veh = 0; }
      if (!r.veh) {
        if (g.loadModel(vm)) { r.veh = g.createVehicle(vm, pos, r.st.h); if (r.veh) { r.vehModel = vm; ownedByUs_.insert(r.veh); g.V(nat::SET_ENTITY_AS_MISSION_ENTITY, {A(r.veh), A(true), A(true)}); g.V(nat::SET_PED_INTO_VEHICLE, {A(r.ped), A(r.veh), A(r.st.sd < -1 ? -1 : r.st.sd)}); logf("spawned vehicle %d for #%d", r.veh, id); } g.releaseModel(vm); }
      } else {
        Vec3 cur = g.coords(r.veh), vel = {r.st.vx, r.st.vy, r.st.vz}; float d = dist3d(cur, pos);
        if (d > 8.0f) { g.setCoords(r.veh, pos); g.setHeading(r.veh, r.st.h); g.setVelocity(r.veh, vel); }
        else { Vec3 fix = {vel.x + (pos.x - cur.x) * 3.0f, vel.y + (pos.y - cur.y) * 3.0f, vel.z + (pos.z - cur.z) * 3.0f}; g.setVelocity(r.veh, fix); if (now - r.lastTask > 100) { g.setHeading(r.veh, r.st.h); r.lastTask = now; } }
        if (g.vehicleOf(r.ped) != r.veh) g.V(nat::SET_PED_INTO_VEHICLE, {A(r.ped), A(r.veh), A(r.st.sd < -1 ? -1 : r.st.sd)});
      }
      continue;
    }
    if (r.veh) { ownedByUs_.erase(r.veh); g.del(r.veh); r.veh = 0; g.V(nat::CLEAR_PED_TASKS_IMMEDIATELY, {A(r.ped)}); g.setCoords(r.ped, pos); }
    // on foot
    if ((r.st.fl & F_DEAD) && !r.dead) { g.V(nat::SET_ENTITY_HEALTH, {A(r.ped), A(0)}); r.dead = true; }
    if (!(r.st.fl & F_DEAD) && r.dead) { g.V(nat::CLEAR_PED_TASKS_IMMEDIATELY, {A(r.ped)}); g.V(nat::SET_ENTITY_HEALTH, {A(r.ped), A(200)}); g.setCoords(r.ped, pos); r.dead = false; }
    if (r.dead) continue;
    bool crouch = (r.st.fl & F_CROUCH) != 0; if (crouch != r.crouch) { g.V(nat::SET_PED_STEALTH_MOVEMENT, {A(r.ped), A(crouch), A(0)}); r.crouch = crouch; }
    if ((r.st.fl & F_RAGDOLL) && !(r.lastFl & F_RAGDOLL)) g.V(nat::SET_PED_TO_RAGDOLL, {A(r.ped), A(1500), A(1500), A(0), A(false), A(false), A(false)});
    bool aim = (r.st.fl & (F_AIM | F_SHOOT)) != 0 && r.st.wp != 0 && r.st.wp != kUnarmed;
    Vec3 vel = {r.st.vx, r.st.vy, r.st.vz}; Vec3 target = pos;
    if (!(r.st.fl & F_RAGDOLL)) moveActor(g, r.ped, target, vel, r.st.h, now, r.lastTask, r.moving, aim);
    if (aim) {
      float hr = r.st.h * kPi / 180.0f; Vec3 at = {pos.x - std::sin(hr) * 20.0f, pos.y + std::cos(hr) * 20.0f, pos.z + 0.4f};
      if (r.st.fl & F_SHOOT) g.V(nat::SET_PED_SHOOTS_AT_COORD, {A(r.ped), A(at.x), A(at.y), A(at.z), A(true)});
      else if (now - r.spawnAt > 300) g.V(nat::TASK_AIM_GUN_AT_COORD, {A(r.ped), A(at.x), A(at.y), A(at.z), A(600), A(false), A(false)});
    }
    r.lastFl = r.st.fl;
  }
}

void Client::updateEntities(Game& g, int me, double now) {
  (void)me; int spawned = 0;
  for (auto it = eps_.begin(); it != eps_.end();) {
    EPup& e = it->second;
    if (now - e.lastSeen > 2200) { if (e.handle) { ownedByUs_.erase(e.handle); g.del(e.handle); } it = eps_.erase(it); continue; }
    ++it; const ERow& r = e.row; Vec3 pos = {r.x, r.y, r.z}, vel = {r.vx, r.vy, r.vz};
    if (!e.handle) {
      if (spawned >= 2 || !g.modelValid(r.m)) continue;
      if (e.spawnAt == 0) e.spawnAt = now;
      if (!g.loadModel(r.m)) { if (now - e.spawnAt > 10000) { e.lastSeen = -1e9; } continue; }
      e.handle = r.k == 2 ? g.createVehicle(r.m, pos, r.h) : g.createPed(r.m, pos, r.h); spawned++;
      if (!e.handle) { e.lastSeen = -1e9; g.releaseModel(r.m); continue; }
      ownedByUs_.insert(e.handle); g.V(nat::SET_ENTITY_AS_MISSION_ENTITY, {A(e.handle), A(true), A(true)});
      if (r.k == 1) { g.V(nat::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS, {A(e.handle), A(true)}); g.V(nat::SET_ENTITY_INVINCIBLE, {A(e.handle), A(true)}); g.V(nat::SET_PED_CAN_RAGDOLL, {A(e.handle), A(false)}); g.V(nat::SET_PED_RANDOM_COMPONENT_VARIATION, {A(e.handle), A(0)}); g.V(nat::SET_PED_KEEP_TASK, {A(e.handle), A(true)}); }
      else { int p = r.a & 0xFF, s = (r.a >> 8) & 0xFF; g.V(nat::SET_VEHICLE_COLOURS, {A(e.handle), A(p), A(s)}); }
      g.releaseModel(r.m); continue;
    }
    if (!g.exists(e.handle)) { ownedByUs_.erase(e.handle); e.handle = 0; e.spawnAt = 0; continue; }
    if (r.k == 2) {
      Vec3 cur = g.coords(e.handle); float d = dist3d(cur, pos);
      if (d > 8.0f) { g.setCoords(e.handle, pos); g.setHeading(e.handle, r.h); g.setVelocity(e.handle, vel); }
      else { g.setVelocity(e.handle, {vel.x + (pos.x - cur.x) * 3.0f, vel.y + (pos.y - cur.y) * 3.0f, vel.z + (pos.z - cur.z) * 3.0f}); if (now - e.lastTask > 100) { g.setHeading(e.handle, r.h); e.lastTask = now; } }
    } else {
      bool inVeh = (r.fl & F_VEH) && r.b != 0; auto vi = inVeh ? eps_.find(r.b) : eps_.end();
      if (inVeh && vi != eps_.end() && vi->second.handle) { if (!e.seated) { g.V(nat::SET_PED_INTO_VEHICLE, {A(e.handle), A(vi->second.handle), A(((r.fl >> 12) & 0xF) - 2)}); e.seated = true; } continue; }
      if (e.seated) { g.V(nat::CLEAR_PED_TASKS_IMMEDIATELY, {A(e.handle)}); g.setCoords(e.handle, pos); e.seated = false; }
      if (inVeh) continue;
      moveActor(g, e.handle, pos, vel, r.h, now, e.lastTask, e.moving, false);
    }
  }
}

// ------------------------------------------------------------------ host: stream the ambient world to everyone else
void Client::streamEntities(Game& g, int me, double now) {
  if (now - lastStream_ < 1000.0 / entRate_) return; lastStream_ = now;
  std::set<int> seen; std::vector<std::string> rows;
  auto lid = [&](int h) { auto it = lidOf_.find(h); if (it == lidOf_.end()) { it = lidOf_.emplace(h, nextLid_).first; nextLid_ = nextLid_ % 65535 + 1; } lidSeen_[h] = now; return (myId_.load() << 16) | it->second; };
  int myVeh = g.vehicleOf(me);
  auto scan = [&](int kind) {
    int buf[2 * 40 + 2]; memset(buf, 0, sizeof buf); buf[0] = 40;
    int cnt = kind == 1 ? (int)g.R(nat::GET_PED_NEARBY_PEDS, {A(me), P(buf), A(-1)}) : (int)g.R(nat::GET_PED_NEARBY_VEHICLES, {A(me), P(buf)});
    if (cnt < 0) cnt = 0; if (cnt > 40) cnt = 40;
    for (int i = 1; i <= 2 * cnt + 1; i++) {
      int h = buf[i]; if (h <= 0 || h == me || h == myVeh || ownedByUs_.count(h) || seen.count(h) || !g.exists(h)) continue;
      if ((int)g.R(nat::GET_ENTITY_TYPE, {A(h)}) != kind) continue;
      if (kind == 1 && (g.flag(nat::IS_PED_A_PLAYER, h) || (g.R(nat::IS_PED_DEAD_OR_DYING, {A(h), A(true)}) & 1))) continue;
      seen.insert(h); Vec3 p = g.coords(h), v = g.velocity(h); int gid = lid(h), fl = 0, a = 0, b = 0;
      if (kind == 1) { a = g.health(h); int veh = g.vehicleOf(h); if (veh && g.exists(veh)) { fl |= F_VEH | (((g.seatOf(h, veh) + 2) & 0xF) << 12); b = lid(veh); } }
      else { int pc = 0, sc = 0; g.V(nat::GET_VEHICLE_COLOURS, {A(h), P(&pc), P(&sc)}); a = (pc & 0xFF) | ((sc & 0xFF) << 8); }
      rows.push_back(fmt("[%d,%d,%u,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%d,%d,%d]", gid, kind, g.model(h), p.x, p.y, p.z, g.heading(h), v.x, v.y, v.z, fl, a, b));
    }
  };
  scan(1); scan(2);
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
  for (auto& kv : remotes_) { Remote& r = kv.second; if (!engineGone) { if (r.ped) g.del(r.ped); if (r.veh) g.del(r.veh); } r.ped = 0; r.veh = 0; r.curWeapon = 0xFFFFFFFFu; r.appliedApp = false; r.spawnAt = 0; r.moving = false; r.dead = false; r.crouch = false; }
  if (!engineGone) remotes_.clear();
  for (auto& kv : eps_) if (!engineGone && kv.second.handle) g.del(kv.second.handle);
  eps_.clear(); ownedByUs_.clear(); lidOf_.clear(); lidSeen_.clear(); clockPaused_ = engineGone ? false : clockPaused_; appliedWeather_.clear();
  if (engineGone) popSuppressed_ = false;
}
}  // namespace mp
