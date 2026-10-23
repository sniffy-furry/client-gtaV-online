// Simulated game world implementing the natives the client uses (host tests only).
#pragma once
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "../src/game.h"
namespace mp {
struct MEnt { int type = 1; uint32_t model = 0; Vec3 pos, vel; float h = 0; int health = 200; bool ambient = false, puppet = false, exists = true; int comps[12][3]; int props[8][2]; uint32_t weapon = 0; int veh = 0, seat = -2; bool going = false; Vec3 goT; float goSp = 0; int col[2] = {0, 0}; int tasks = 0;
  MEnt() { memset(comps, 0, sizeof comps); for (auto& p : props) { p[0] = -1; p[1] = 0; } } };
class MockGame : public Natives {
 public:
  std::mutex mu; std::map<int, MEnt> ents; int playerHandle = 1000, nextHandle = 2000;
  std::set<uint32_t> loaded; std::map<uint32_t, int> loadCalls;
  bool clockPaused = false; int clockH = -1, clockM = -1, clockS = -1; std::string weather; int pedBudget = -1, vehBudget = -1, densityCalls = 0, clockSets = 0;
  int wanted = 0, feedPosts = 0, shootCalls = 0, aimCalls = 0, animCalls = 0, ragdollCalls = 0; std::string lastAnimDict, lastAnimClip; int created = 0, deleted = 0; std::set<uint64_t> unknown;
  int flags = 0; /* bit mask for IS_* natives: 1 aim,2 shoot,4 reload,8 duck,16 ragdoll,32 sprint,128 swim,256 climb,512 cover,1024 dead */
  MockGame() { MEnt p; p.type = 1; ents[playerHandle] = p; }
  MEnt& player() { return ents[playerHandle]; }
  void step(float dt) {
    std::lock_guard<std::mutex> l(mu);
    for (auto& kv : ents) { MEnt& e = kv.second; if (!e.exists) continue;
      if (e.going) { Vec3 d = {e.goT.x - e.pos.x, e.goT.y - e.pos.y, 0}; float n = std::hypot(d.x, d.y); float sp = e.goSp * 2.0f; if (n < 0.3f) { e.going = false; e.vel = {}; } else { e.vel = {d.x / n * sp, d.y / n * sp, 0}; } }
      e.pos.x += e.vel.x * dt; e.pos.y += e.vel.y * dt; e.pos.z += e.vel.z * dt; }
  }
  Vec3 posOf(int h) { std::lock_guard<std::mutex> l(mu); auto it = ents.find(h); return it == ents.end() ? Vec3{} : it->second.pos; }
  std::vector<int> puppetsOf(int type) { std::lock_guard<std::mutex> l(mu); std::vector<int> v; for (auto& kv : ents) if (kv.second.puppet && kv.second.exists && kv.second.type == type) v.push_back(kv.first); return v; }
  bool call(uint64_t hash, std::initializer_list<uint64_t> args, uint64_t* ret) override {
    std::lock_guard<std::mutex> l(mu); std::vector<uint64_t> a(args);
    auto I = [&](size_t i) { return i < a.size() ? (int)(uint32_t)a[i] : 0; };
    auto Fl = [&](size_t i) { return i < a.size() ? F(a[i]) : 0.f; };
    auto Pt = [&](size_t i) { return i < a.size() ? (void*)(uintptr_t)a[i] : nullptr; };
    auto E = [&](size_t i) -> MEnt* { auto it = ents.find(I(i)); return (it != ents.end() && it->second.exists) ? &it->second : nullptr; };
    auto setF = [&](int slot, float f) { uint32_t b; memcpy(&b, &f, 4); ret[slot] = b; };
    using namespace nat;
    switch (hash) {
      case PLAYER_PED_ID: ret[0] = playerHandle; return true;
      case DOES_ENTITY_EXIST: ret[0] = E(0) ? 1 : 0; return true;
      case GET_ENTITY_COORDS: if (auto* e = E(0)) { setF(0, e->pos.x); setF(1, e->pos.y); setF(2, e->pos.z); } return true;
      case GET_ENTITY_VELOCITY: if (auto* e = E(0)) { setF(0, e->vel.x); setF(1, e->vel.y); setF(2, e->vel.z); } return true;
      case GET_ENTITY_HEADING: if (auto* e = E(0)) setF(0, e->h); return true;
      case GET_ENTITY_HEALTH: if (auto* e = E(0)) ret[0] = e->health; return true;
      case GET_ENTITY_MODEL: if (auto* e = E(0)) ret[0] = e->model; return true;
      case GET_ENTITY_TYPE: if (auto* e = E(0)) ret[0] = e->type; return true;
      case SET_ENTITY_COORDS_NO_OFFSET: if (auto* e = E(0)) { e->pos = {Fl(1), Fl(2), Fl(3)}; e->going = false; } return true;
      case SET_ENTITY_HEADING: if (auto* e = E(0)) e->h = Fl(1); return true;
      case SET_ENTITY_VELOCITY: if (auto* e = E(0)) { e->vel = {Fl(1), Fl(2), Fl(3)}; e->going = false; } return true;
      case SET_ENTITY_AS_MISSION_ENTITY: case SET_BLOCKING_OF_NON_TEMPORARY_EVENTS: case SET_ENTITY_INVINCIBLE: case SET_PED_CAN_RAGDOLL: case SET_PED_KEEP_TASK: case SET_PED_STEALTH_MOVEMENT: return true;
      case SET_PED_RANDOM_COMPONENT_VARIATION: return true;
      case DELETE_ENTITY: { int* p = (int*)Pt(0); auto it = ents.find(*p); if (it != ents.end()) { it->second.exists = false; deleted++; } return true; }
      case IS_MODEL_IN_CDIMAGE: ret[0] = a[0] != 0 ? 1 : 0; return true;
      case REQUEST_MODEL: loadCalls[(uint32_t)a[0]]++; return true;
      case HAS_MODEL_LOADED: ret[0] = loadCalls[(uint32_t)a[0]] >= 2 ? 1 : 0; return true;
      case SET_MODEL_AS_NO_LONGER_NEEDED: loadCalls[(uint32_t)a[0]] = 0; return true;
      case CREATE_PED: case CREATE_VEHICLE: { MEnt e; e.type = hash == CREATE_PED ? 1 : 2; e.model = (uint32_t)(hash == CREATE_PED ? a[1] : a[0]); size_t o = hash == CREATE_PED ? 2 : 1; e.pos = {Fl(o), Fl(o + 1), Fl(o + 2)}; e.h = Fl(o + 3); e.puppet = true; int h = nextHandle++; ents[h] = e; created++; ret[0] = h; return true; }
      case GET_SELECTED_PED_WEAPON: if (auto* e = E(0)) ret[0] = e->weapon; return true;
      case GET_AMMO_IN_PED_WEAPON: ret[0] = E(0) && E(0)->weapon ? 40 : 0; return true;
      case GET_ENTITY_QUATERNION: case SET_ENTITY_QUATERNION: case SET_VEHICLE_ENGINE_ON: case GET_VEHICLE_LIGHTS_STATE: case SET_VEHICLE_LIGHTS: case SET_VEHICLE_FULLBEAM: case SET_VEHICLE_SIREN: case START_VEHICLE_HORN: case SET_VEHICLE_BODY_HEALTH: case SET_VEHICLE_ENGINE_HEALTH: case SET_VEHICLE_DOOR_OPEN: case SET_VEHICLE_DOOR_SHUT: case SET_VEHICLE_TYRE_BURST: case GET_VEHICLE_DOOR_ANGLE_RATIO: case IS_VEHICLE_TYRE_BURST: case GET_IS_VEHICLE_ENGINE_RUNNING: case IS_VEHICLE_SIREN_ON: case IS_HORN_ACTIVE: case GET_VEHICLE_BODY_HEALTH: case GET_VEHICLE_ENGINE_HEALTH: return true;
      case SET_ENTITY_PROOFS: case EXPLODE_VEHICLE: case START_ENTITY_FIRE: case STOP_ENTITY_FIRE: case IS_ENTITY_ON_FIRE: case IS_ENTITY_DEAD: return true;
      case GET_ENTITY_POPULATION_TYPE: if (auto* e = E(0)) ret[0] = e->ambient ? 5 : 7; return true;
      case GET_PLAYER_WANTED_LEVEL: ret[0] = wanted; return true;
      case SET_PLAYER_WANTED_LEVEL: wanted = (int)a[1]; return true;
      case END_TEXT_COMMAND_THEFEED_POST_TICKER: feedPosts++; return true;
      case GET_GAMEPLAY_CAM_ROT: case SET_PLAYER_WANTED_LEVEL_NOW: case BEGIN_TEXT_COMMAND_THEFEED_POST: case ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME: return true;
      case IS_PED_IN_ANY_VEHICLE: if (auto* e = E(0)) ret[0] = e->veh != 0; return true;
      case GET_VEHICLE_PED_IS_IN: if (auto* e = E(0)) ret[0] = e->veh; return true;
      case GET_PED_IN_VEHICLE_SEAT: { for (auto& kv : ents) if (kv.second.veh == I(0) && kv.second.seat == I(1) && kv.second.exists) { ret[0] = kv.first; return true; } return true; }
      case GET_PLAYER_INDEX: ret[0] = 0; return true;
      case IS_PLAYER_FREE_AIMING: ret[0] = flags & 1; return true;
      case IS_PED_SHOOTING: ret[0] = (flags & 2) != 0; return true;
      case IS_PED_RELOADING: ret[0] = (flags & 4) != 0; return true;
      case IS_PED_DUCKING: ret[0] = (flags & 8) != 0; return true;
      case IS_PED_RAGDOLL: ret[0] = (flags & 16) != 0; return true;
      case IS_PED_SPRINTING: ret[0] = (flags & 32) != 0; return true;
      case IS_PED_SWIMMING: ret[0] = (flags & 128) != 0; return true;
      case IS_PED_CLIMBING: ret[0] = (flags & 256) != 0; return true;
      case IS_PED_IN_COVER: ret[0] = (flags & 512) != 0; return true;
      case IS_PED_DEAD_OR_DYING: ret[0] = (E(0) && E(0)->health <= 0) || ((flags & 1024) && I(0) == playerHandle); return true;
      case IS_PED_A_PLAYER: ret[0] = I(0) == playerHandle; return true;
      case GET_PED_DRAWABLE_VARIATION: if (auto* e = E(0)) ret[0] = (uint32_t)e->comps[I(1)][0]; return true;
      case GET_PED_TEXTURE_VARIATION: if (auto* e = E(0)) ret[0] = (uint32_t)e->comps[I(1)][1]; return true;
      case GET_PED_PALETTE_VARIATION: if (auto* e = E(0)) ret[0] = (uint32_t)e->comps[I(1)][2]; return true;
      case GET_PED_PROP_INDEX: if (auto* e = E(0)) ret[0] = (uint32_t)e->props[I(1)][0]; return true;
      case GET_PED_PROP_TEXTURE_INDEX: if (auto* e = E(0)) ret[0] = (uint32_t)e->props[I(1)][1]; return true;
      case SET_PED_COMPONENT_VARIATION: if (auto* e = E(0)) { e->comps[I(1)][0] = I(2); e->comps[I(1)][1] = I(3); e->comps[I(1)][2] = I(4); } return true;
      case SET_PED_PROP_INDEX: if (auto* e = E(0)) { e->props[I(1)][0] = I(2); e->props[I(1)][1] = I(3); } return true;
      case CLEAR_PED_PROP: if (auto* e = E(0)) e->props[I(1)][0] = -1; return true;
      case PAUSE_CLOCK: clockPaused = a[0] & 1; return true;
      case SET_CLOCK_TIME: clockH = I(0); clockM = I(1); clockS = I(2); clockSets++; return true;
      case CLEAR_OVERRIDE_WEATHER: return true;
      case SET_WEATHER_TYPE_NOW_PERSIST: case SET_OVERRIDE_WEATHER: weather = (const char*)Pt(0); return true;
      case SET_PED_POPULATION_BUDGET: pedBudget = I(0); return true;
      case SET_VEHICLE_POPULATION_BUDGET: vehBudget = I(0); return true;
      case SET_PED_DENSITY_MULTIPLIER_THIS_FRAME: case SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME: case SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME: case SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME: case SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME: densityCalls++; return true;
      case TASK_GO_STRAIGHT_TO_COORD: if (auto* e = E(0)) { e->going = true; e->goT = {Fl(1), Fl(2), Fl(3)}; e->goSp = Fl(4); e->tasks++; } return true;
      case CLEAR_PED_TASKS: case CLEAR_PED_TASKS_IMMEDIATELY: if (auto* e = E(0)) { e->going = false; e->vel = {}; } return true;
      case SET_PED_INTO_VEHICLE: if (auto* e = E(0)) { e->veh = I(1); e->seat = I(2); } return true;
      case GIVE_WEAPON_TO_PED: if (auto* e = E(0)) e->weapon = (uint32_t)a[1]; return true;
      case SET_CURRENT_PED_WEAPON: if (auto* e = E(0)) e->weapon = (uint32_t)a[1]; return true;
      case REMOVE_ALL_PED_WEAPONS: if (auto* e = E(0)) e->weapon = 0; return true;
      case SET_ENTITY_HEALTH: if (auto* e = E(0)) e->health = I(1); return true;
      case SET_PED_TO_RAGDOLL: ragdollCalls++; return true;
      case SET_PED_SHOOTS_AT_COORD: shootCalls++; return true;
      case TASK_AIM_GUN_AT_COORD: aimCalls++; return true;
      case SET_VEHICLE_COLOURS: if (auto* e = E(0)) { e->col[0] = I(1); e->col[1] = I(2); } return true;
      case GET_VEHICLE_COLOURS: if (auto* e = E(0)) { *(int*)Pt(1) = e->col[0]; *(int*)Pt(2) = e->col[1]; } return true;
      case REQUEST_ANIM_DICT: return true;
      case HAS_ANIM_DICT_LOADED: ret[0] = 1; return true;
      case TASK_PLAY_ANIM: animCalls++; lastAnimDict = (const char*)Pt(1); lastAnimClip = (const char*)Pt(2); return true;
      case GET_PED_NEARBY_PEDS: case GET_PED_NEARBY_VEHICLES: {
        int* buf = (int*)Pt(1); int want = hash == GET_PED_NEARBY_PEDS ? 1 : 2, n = 0, max = buf[0]; Vec3 me = ents[playerHandle].pos;
        for (auto& kv : ents) { MEnt& e = kv.second; if (!e.exists || kv.first == playerHandle || e.type != want) continue; if (std::hypot(e.pos.x - me.x, e.pos.y - me.y) > 150) continue; if (n >= max) break; buf[n * 2 + 2] = kv.first; n++; }
        ret[0] = n; return true; }
      default: unknown.insert(hash); return false;
    }
  }
};
}  // namespace mp
