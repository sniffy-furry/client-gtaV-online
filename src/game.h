// Game-side API. Everything here runs on the game thread inside the engine hook tick.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "natives.h"
namespace mp {
struct Vec3 { float x = 0, y = 0, z = 0; };
inline float dist2d(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }
inline float dist3d(const Vec3& a, const Vec3& b) { return std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z)); }

// A native-call backend: the real engine on Android (hook), a simulated world in the host tests.
class Natives {
 public:
  virtual ~Natives() {}
  // ret must point at >= 8 zeroed qwords. Returns false if the engine rejected the call.
  virtual bool call(uint64_t hash, std::initializer_list<uint64_t> args, uint64_t* ret) = 0;
};
inline uint64_t A(int v) { return (uint64_t)(uint32_t)v; }
inline uint64_t A(unsigned v) { return (uint64_t)v; }
inline uint64_t A(bool v) { return v ? 1u : 0u; }
inline uint64_t A(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
inline uint64_t P(const void* p) { return (uint64_t)(uintptr_t)p; }
inline float F(uint64_t r) { uint32_t b = (uint32_t)r; float f; memcpy(&f, &b, 4); return f; }
inline uint32_t joaat(const char* s) {
  uint32_t h = 0; for (; *s; ++s) { char c = *s; if (c >= 'A' && c <= 'Z') c += 32; h += (uint8_t)c; h += h << 10; h ^= h >> 6; } h += h << 3; h ^= h >> 11; h += h << 15; return h;
}

class Game {
 public:
  explicit Game(Natives& n) : n_(n) {}
  uint64_t R(uint64_t hash, std::initializer_list<uint64_t> a = {}) { uint64_t r[8] = {0}; ok_ = n_.call(hash, a, r); return r[0]; }
  bool lastOk() const { return ok_; }
  void V(uint64_t hash, std::initializer_list<uint64_t> a = {}) { uint64_t r[8] = {0}; ok_ = n_.call(hash, a, r); }
  Vec3 vec(uint64_t hash, std::initializer_list<uint64_t> a) {
    uint64_t r[8] = {0}; ok_ = n_.call(hash, a, r); Vec3 v;
    v.x = F(r[0]); v.y = F(r[1]); v.z = F(r[2]);
    if (v.x == 0 && v.y == 0 && v.z == 0) { // packed layout fallback (x,y,z as consecutive floats in the first 12 bytes)
      float p[3]; memcpy(p, r, 12); if (p[0] != 0 || p[1] != 0 || p[2] != 0) { v.x = p[0]; v.y = p[1]; v.z = p[2]; }
    }
    return v;
  }
  // ---- entities
  int playerPed() { return (int)R(nat::PLAYER_PED_ID); }
  bool exists(int h) { return h != 0 && R(nat::DOES_ENTITY_EXIST, {A(h)}) & 1; }
  Vec3 coords(int h) { return vec(nat::GET_ENTITY_COORDS, {A(h), A(true)}); }
  Vec3 velocity(int h) { return vec(nat::GET_ENTITY_VELOCITY, {A(h)}); }
  float heading(int h) { return F(R(nat::GET_ENTITY_HEADING, {A(h)})); }
  int health(int h) { return (int)R(nat::GET_ENTITY_HEALTH, {A(h)}); }
  uint32_t model(int h) { return (uint32_t)R(nat::GET_ENTITY_MODEL, {A(h)}); }
  void setCoords(int h, const Vec3& p) { V(nat::SET_ENTITY_COORDS_NO_OFFSET, {A(h), A(p.x), A(p.y), A(p.z), A(false), A(false), A(false)}); }
  void setHeading(int h, float d) { V(nat::SET_ENTITY_HEADING, {A(h), A(d)}); }
  void setVelocity(int h, const Vec3& v) { V(nat::SET_ENTITY_VELOCITY, {A(h), A(v.x), A(v.y), A(v.z)}); }
  void del(int h) { int hh = h; V(nat::SET_ENTITY_AS_MISSION_ENTITY, {A(h), A(true), A(true)}); V(nat::DELETE_ENTITY, {P(&hh)}); }
  // ---- models / spawning
  bool modelValid(uint32_t m) { return R(nat::IS_MODEL_IN_CDIMAGE, {A(m)}) & 1; }
  bool loadModel(uint32_t m) { V(nat::REQUEST_MODEL, {A(m)}); return R(nat::HAS_MODEL_LOADED, {A(m)}) & 1; }
  void releaseModel(uint32_t m) { V(nat::SET_MODEL_AS_NO_LONGER_NEEDED, {A(m)}); }
  int createPed(uint32_t m, const Vec3& p, float h) { return (int)R(nat::CREATE_PED, {A(26), A(m), A(p.x), A(p.y), A(p.z), A(h), A(false), A(false)}); }
  int createVehicle(uint32_t m, const Vec3& p, float h) { return (int)R(nat::CREATE_VEHICLE, {A(m), A(p.x), A(p.y), A(p.z), A(h), A(false), A(false), A(false)}); }
  // ---- peds
  uint32_t weaponOf(int ped) { return (uint32_t)R(nat::GET_SELECTED_PED_WEAPON, {A(ped)}); }
  int ammoOf(int ped, uint32_t w) { return (int)R(nat::GET_AMMO_IN_PED_WEAPON, {A(ped), A(w)}); }
  int vehicleOf(int ped) { return (int)R(nat::GET_VEHICLE_PED_IS_IN, {A(ped), A(false)}); }
  int seatOf(int ped, int veh) { for (int s = -1; s < 8; s++) if ((int)R(nat::GET_PED_IN_VEHICLE_SEAT, {A(veh), A(s), A(0)}) == ped) return s; return -2; }
  bool flag(uint64_t native, int ped) { return R(native, {A(ped)}) & 1; }
 private:
  Natives& n_; bool ok_ = true;
};
}  // namespace mp
