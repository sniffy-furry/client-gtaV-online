#include "client.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "sha256.h"

namespace mp {
// ------------------------------------------------------------------ logging
static std::mutex g_logmu; static std::string g_logpath; static std::function<void(const std::string&)> g_sink;
void set_log(const std::string& path, std::function<void(const std::string&)> sink) { std::lock_guard<std::mutex> l(g_logmu); g_logpath = path; g_sink = std::move(sink); }
double now_ms() { using namespace std::chrono; return (double)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0; }
void logf(const char* fmt, ...) {
  char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  std::lock_guard<std::mutex> l(g_logmu);
  if (!g_logpath.empty()) { if (FILE* f = fopen(g_logpath.c_str(), "a")) { fprintf(f, "%s\n", b); fclose(f); } }
  if (g_sink) g_sink(b);
}
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static std::string fmtf(const char* fmt, ...) { char b[1500]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); return b; }

// ------------------------------------------------------------------ http (mods)
static bool http_get(const std::string& host, int port, const std::string& path, std::string* body, const std::string& file, int* status, int timeoutMs) {
  addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return false;
  int s = socket(res->ai_family, res->ai_socktype, 0); if (s < 0) { freeaddrinfo(res); return false; }
  int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
  bool ok = connect(s, res->ai_addr, res->ai_addrlen) == 0;
  if (!ok && errno == EINPROGRESS) {
    fd_set w; FD_ZERO(&w); FD_SET(s, &w); timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(s + 1, nullptr, &w, nullptr, &tv) > 0) { int err = 0; socklen_t l = sizeof err; getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l); ok = err == 0; }
  }
  freeaddrinfo(res);
  if (!ok) { close(s); return false; }
  fcntl(s, F_SETFL, fl); timeval rt{timeoutMs / 1000, (timeoutMs % 1000) * 1000}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
  std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
  if (send(s, req.data(), req.size(), 0) != (ssize_t)req.size()) { close(s); return false; }
  std::string head; bool headDone = false; FILE* out = nullptr; std::string part;
  if (!file.empty()) { part = file + ".part"; out = fopen(part.c_str(), "wb"); if (!out) { close(s); return false; } }
  char buf[8192]; ssize_t n; int st = 0; bool good = true;
  while ((n = recv(s, buf, sizeof buf, 0)) > 0) {
    if (!headDone) {
      head.append(buf, n); size_t p = head.find("\r\n\r\n"); if (p == std::string::npos) { if (head.size() > 16384) { good = false; break; } continue; }
      st = atoi(head.c_str() + 9); headDone = true; std::string rest = head.substr(p + 4);
      if (!rest.empty()) { if (out) fwrite(rest.data(), 1, rest.size(), out); else if (body) body->append(rest); }
    } else { if (out) fwrite(buf, 1, n, out); else if (body) body->append(buf, n); }
  }
  close(s); if (out) fclose(out);
  if (status) *status = st;
  if (!good || !headDone) { if (!part.empty()) unlink(part.c_str()); return false; }
  if (!file.empty()) { if (st == 200) rename(part.c_str(), file.c_str()); else unlink(part.c_str()); }
  return true;
}
static std::string sha256_file(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return ""; Sha256 h; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) h.update(b, n); fclose(f); return h.hex(); }
static std::string trim(std::string s) { while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back(); size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++; return s.substr(i); }

// ------------------------------------------------------------------ config + mods
bool Client::loadCfg() {
  std::string path = env_.dir + "/mp.cfg"; FILE* f = fopen(path.c_str(), "r");
  if (!f) { logf("no %s/mp.cfg (server=...) -> inactive", env_.dir.c_str()); return false; }
  char line[512];
  while (fgets(line, sizeof line, f)) {
    std::string l = trim(line); if (l.empty() || l[0] == '#') continue; size_t eq = l.find('='); if (eq == std::string::npos) continue;
    std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
    if (k == "server") { size_t c = v.rfind(':'); if (c != std::string::npos) { cfg_.host = v.substr(0, c); cfg_.port = atoi(v.c_str() + c + 1); } else cfg_.host = v; }
    else if (k == "port") cfg_.port = atoi(v.c_str()); else if (k == "http") cfg_.http = atoi(v.c_str()); else if (k == "name") cfg_.name = v;
  }
  fclose(f);
  std::string n; for (char c : cfg_.name) if (isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '.' || c == '-') n += c; cfg_.name = n.empty() ? "Player" : n.substr(0, 24);
  if (cfg_.http <= 0) cfg_.http = cfg_.port + 1;
  cfg_.ok = !cfg_.host.empty(); if (cfg_.ok) logf("server %s:%d name %s", cfg_.host.c_str(), cfg_.port, cfg_.name.c_str());
  return cfg_.ok;
}

bool Client::syncMods(std::string& modsField) {
  modsField = Sha256::of("");
  std::string body; int st = 0;
  if (!http_get(cfg_.host, cfg_.http, "/manifest.json", &body, "", &st, 4000) || st != 200) { logf("mod server unreachable; will still try UDP"); return true; }
  JV m; if (!JParser::parse(body.data(), body.size(), m) || !m.at("mods").isArr()) { logf("manifest: unreadable"); return true; }
  std::string modsDir = env_.dir + "/Mods"; mkdir(modsDir.c_str(), 0775);
  std::map<std::string, std::string> installed; // name -> sha
  if (FILE* f = fopen((modsDir + "/mp_installed.txt").c_str(), "r")) { char ln[512]; while (fgets(ln, sizeof ln, f)) { std::string l = trim(ln); size_t t = l.find('\t'); if (t != std::string::npos) installed[l.substr(0, t)] = l.substr(t + 1); } fclose(f); }
  std::string joined; bool changed = false;
  for (const JV& e : m.at("mods").a) {
    std::string name = e.at("name").str(), sha = e.at("sha256").str(); bool req = e.at("required").num() != 0;
    if (name.empty() || sha.size() != 64 || name.find('/') != std::string::npos || name.find("..") != std::string::npos) { logf("manifest: skipping bad entry '%s'", name.c_str()); continue; }
    if (!req) continue;
    if (!joined.empty()) joined += "\n"; joined += name + ":" + sha;
    auto it = installed.find(name); if (it != installed.end() && it->second == sha) continue;
    std::string inbox = modsDir + "/inbox"; mkdir(inbox.c_str(), 0775); std::string dst = inbox + "/" + name;
    if (sha256_file(dst) == sha) { changed = true; continue; }
    logf("mod: downloading %s (%.0f bytes, %s)", name.c_str(), e.at("size").num(), e.at("kind").str("addon").c_str());
    int hs = 0; if (!http_get(cfg_.host, cfg_.http, "/mods/" + name, nullptr, dst, &hs, 30000) || hs != 200 || sha256_file(dst) != sha) { logf("mod: %s failed (http %d, hash %s)", name.c_str(), hs, sha256_file(dst).substr(0, 12).c_str()); unlink(dst.c_str()); return false; }
    changed = true;
  }
  if (changed) { logf("mods changed: archives are in %s/inbox. Import them in the launcher, restart the game, then reconnect.", modsDir.c_str()); return false; }
  modsField = Sha256::of(joined);
  return true;
}

// ------------------------------------------------------------------ network
bool Client::openUdp() {
  addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; addrinfo* res = nullptr;
  if (getaddrinfo(cfg_.host.c_str(), std::to_string(cfg_.port).c_str(), &hints, &res) != 0 || !res) { logf("cannot resolve %s", cfg_.host.c_str()); return false; }
  sock_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock_ < 0 || connect(sock_, res->ai_addr, res->ai_addrlen) != 0) { logf("cannot open UDP socket"); freeaddrinfo(res); if (sock_ >= 0) close(sock_); sock_ = -1; return false; }
  freeaddrinfo(res); fcntl(sock_, F_SETFL, fcntl(sock_, F_GETFL, 0) | O_NONBLOCK);
  int rb = 1 << 18; setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
  return true;
}
void Client::sendRaw(const std::string& s) { if (sock_ >= 0 && s.size() <= 1400) { if (::send(sock_, s.data(), s.size(), 0) >= 0) lastTx_ = now_ms(); } }
void Client::queueOut(const std::string& s) { std::lock_guard<std::mutex> l(sh_.mu); if (sh_.outbox.size() < 200) sh_.outbox.push_back(s); }

void Client::handle(const std::string& line) {
  JV m; if (!JParser::parse(line.data(), line.size(), m) || m.t != JV::Obj) return;
  std::string t = m.at("t").str(); double now = now_ms();
  if (t == "ok") {
    bool first = !joined_; myId_ = m.at("id").i(); joined_ = true; lastRx_ = now; if (first) popOwn_ = true;
    serverTickHz_ = std::max(1, std::min(60, m.at("tick").i(10)));
    v2_ = m.at("v").i(1) >= 2; if (v2_) { hostId_ = m.at("host").i(0); pop_ = m.at("pop").str("host"); entRate_ = std::max(1, std::min(20, m.at("er").i(6))); interest_ = m.at("ir").i(300); }
    if (first) logf("joined as #%d%s", myId_.load(), v2_ ? " (protocol v2)" : "");
  } else if (t == "deny") { logf("join denied: %s", m.at("r").str().c_str()); }
  else if (t == "chat") { logf("[chat] <%s> %s", m.at("n").str().c_str(), m.at("m").str().c_str()); std::lock_guard<std::mutex> l(sh_.mu); if (sh_.chatIn.size() < 20) sh_.chatIn.push_back(m.at("n").str() + ": " + m.at("m").str()); }
  else if (t == "pop") { bool o = m.at("m").i(1) != 0; if (o != popOwn_) logf("population: %s", o ? "I own the area around me (local traffic on, streamed to others)" : "another player owns this area (local traffic off, mirroring)"); popOwn_ = o; }
  else if (t == "wl") { std::lock_guard<std::mutex> l(sh_.mu); sh_.wlIn.push_back(m.at("l").i()); }
  else if (t == "w") {
    std::vector<PState> v;
    for (const JV& p : m.at("p").a) {
      PState s; s.id = p.at("i").i(); s.name = p.at("n").str(); s.x = (float)p.at("x").num(); s.y = (float)p.at("y").num(); s.z = (float)p.at("z").num(); s.h = (float)p.at("h").num();
      s.vx = (float)p.at("vx").num(); s.vy = (float)p.at("vy").num(); s.vz = (float)p.at("vz").num(); s.hp = p.at("hp").i(200); s.wp = p.at("wp").u(); s.iv = p.at("iv").i(); s.vm = p.at("vm").str();
      s.fl = p.at("fl").i(0); s.sd = p.at("sd").i(-2); s.am = p.at("am").i(0); s.cp = (float)p.at("cp").num(); s.cy = (float)p.at("cy").num(); for (const JV& x : p.at("vd").a) s.vd.push_back((float)x.num()); s.rv = p.at("rv").i(0); s.rg = p.at("rg").i(0); v.push_back(s);
    }
    std::lock_guard<std::mutex> l(sh_.mu); sh_.players = std::move(v); sh_.playersNew = true;
  } else if (t == "left") { int id = m.at("i").i(); logf("player #%d left", id); std::lock_guard<std::mutex> l(sh_.mu); sh_.left.push_back(id); }
  else if (t == "wt") {
    std::lock_guard<std::mutex> l(sh_.mu); sh_.wsec = m.at("s").i(); sh_.wscale = (float)m.at("sc").num(30); sh_.wfrozen = m.at("fz").i() != 0; sh_.wweather = m.at("w").str("CLEAR"); sh_.wrecv = now; sh_.worldNew = true;
  } else if (t == "host") { int h = m.at("h").i(); if (h != hostId_) logf("world host is now #%d%s", h, h == myId_ ? " (me)" : ""); hostId_ = h; }
  else if (t == "a") {
    App a; a.valid = true; a.dirty = true; const JV& mm = m.at("m"); a.model = mm.isStr() ? joaat(mm.s.c_str()) : mm.u();
    for (const JV& x : m.at("c").a) a.c.push_back(x.i()); for (const JV& x : m.at("p").a) a.p.push_back(x.i());
    std::lock_guard<std::mutex> l(sh_.mu); sh_.apps[m.at("i").i()] = a;
  } else if (t == "wp") { Loadout w; w.valid = true; w.dirty = true; w.w = m.at("w").u(); w.am = m.at("am").i(); std::lock_guard<std::mutex> l(sh_.mu); sh_.loads[m.at("i").i()] = w; }
  else if (t == "an") { AnimEv a{m.at("i").i(), m.at("d").str(), m.at("c").str(), m.at("f").i(), m.at("s").i() != 0}; std::lock_guard<std::mutex> l(sh_.mu); if (sh_.anims.size() < 64) sh_.anims.push_back(a); }
  else if (t == "e") {
    std::vector<ERow> rows;
    for (const JV& r : m.at("e").a) {
      if (!r.isArr() || r.a.size() < 14) continue; ERow e; e.g = r.a[0].i(); e.k = r.a[1].i(); e.m = r.a[2].u(); e.x = (float)r.a[3].num(); e.y = (float)r.a[4].num(); e.z = (float)r.a[5].num(); e.h = (float)r.a[6].num();
      e.vx = (float)r.a[7].num(); e.vy = (float)r.a[8].num(); e.vz = (float)r.a[9].num(); e.fl = r.a[10].i(); e.a = r.a[11].i(); e.b = r.a[12].i(); e.ctl = r.a[13].i(); if (r.a.size() > 14 && r.a[14].isArr()) for (const JV& x : r.a[14].a) e.d.push_back((float)x.num()); rows.push_back(e);
    }
    std::lock_guard<std::mutex> l(sh_.mu); if (sh_.ents.size() < 2000) sh_.ents.insert(sh_.ents.end(), rows.begin(), rows.end());
  } else if (t == "ed") { std::lock_guard<std::mutex> l(sh_.mu); for (const JV& g : m.at("l").a) sh_.edel.push_back(g.i()); }
  else if (t == "take") { std::lock_guard<std::mutex> l(sh_.mu); sh_.take.push_back(m.at("g").i()); }
  else if (t == "own") { std::lock_guard<std::mutex> l(sh_.mu); sh_.owns.push_back({m.at("g").i(), m.at("c").i()}); }
}

void Client::sayFile() {
  std::string path = env_.dir + "/mp_say.txt"; FILE* f = fopen(path.c_str(), "r"); if (!f) return;
  std::vector<std::string> lines; char b[512]; while (fgets(b, sizeof b, f)) { std::string l = trim(b); if (!l.empty()) lines.push_back(l); }
  fclose(f); unlink(path.c_str());
  for (const std::string& l : lines) {
    if (l.rfind("/anim ", 0) == 0) { // /anim <dict> <clip> [flags]
      char d[64] = {0}, c[64] = {0}; int fl = 1; if (sscanf(l.c_str() + 6, "%63s %63s %d", d, c, &fl) >= 2) { sendRaw(fmtf("{\"t\":\"an\",\"d\":\"%s\",\"c\":\"%s\",\"f\":%d,\"s\":1}", jesc(d).c_str(), jesc(c).c_str(), fl)); std::lock_guard<std::mutex> g(sh_.mu); sh_.localAnim.push_back(std::string(d) + "|" + c + "|" + std::to_string(fl)); }
    } else if (l == "/stopanim") { sendRaw("{\"t\":\"an\",\"d\":\"\",\"c\":\"\",\"f\":0,\"s\":0}"); std::lock_guard<std::mutex> g(sh_.mu); sh_.localAnim.push_back("stop"); }
    else sendRaw("{\"t\":\"chat\",\"m\":\"" + jesc(l) + "\"}");
  }
}
void Client::writeStatus(bool force) {
  double now = now_ms(); if (!force && now - lastStatus_ < 2000) return; lastStatus_ = now;
  if (FILE* f = fopen((env_.dir + "/mp_status.txt").c_str(), "w")) {
    fprintf(f, "state=%s\nid=%d\nhost=%d\nserver=%s:%d\nname=%s\nremote_players=%d\nremote_entities=%d\n", joined_ ? "joined" : "connecting", myId_.load(), hostId_.load(), cfg_.host.c_str(), cfg_.port, cfg_.name.c_str(), stats.puppetPlayers.load(), stats.puppetEnts.load());
    fclose(f);
  }
}

void Client::run() {
  if (!loadCfg()) return;
  if (!syncMods(modsField_)) { writeStatus(true); return; }
  if (!openUdp()) return;
  logf("running");
  double lastEpochPoll = 0;
  while (!stop_) {
    double now = now_ms(); char buf[2048]; ssize_t n;
    while ((n = ::recv(sock_, buf, sizeof buf, 0)) > 0) { lastRx_ = now; handle(std::string(buf, (size_t)n)); }
    if (joined_ && now - lastRx_ > 10000) { logf("server timeout, rejoining"); joined_ = false; v2_ = false; hostId_ = 0; }
    if (!joined_) {
      if (now - lastJoin_ >= 2000) { sendRaw(fmtf("{\"t\":\"join\",\"name\":\"%s\",\"v\":2,\"mods\":\"%s\"}", jesc(cfg_.name).c_str(), modsField_.c_str())); lastJoin_ = now; }
    } else {
      if (now - lastStateTx_ >= 1000.0 / serverTickHz_) {
        lastStateTx_ = now; PState s; bool ok = false;
        { std::lock_guard<std::mutex> l(sh_.mu); ok = sh_.localValid && now - sh_.localAt < 800; s = sh_.local; }
        if (ok) {
          std::string vm = s.vm; for (char& c : vm) if (!isalnum((unsigned char)c) && c != '_') c = '_';
          sendRaw(fmtf("{\"t\":\"s\",\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"h\":%.1f,\"hp\":%d,\"vx\":%.2f,\"vy\":%.2f,\"vz\":%.2f,\"wp\":%u,\"iv\":%d,\"vm\":\"%s\",\"fl\":%d,\"sd\":%d,\"am\":%d,\"cp\":%.1f,\"cy\":%.1f%s}", s.x, s.y, s.z, s.h, s.hp, s.vx, s.vy, s.vz, s.wp, s.iv, vm.c_str(), s.fl, s.sd, s.am, s.cp, s.cy, (std::string(s.vdj.empty() ? "" : ",\"vd\":" + s.vdj) + (s.rv ? ",\"rv\":" + std::to_string(s.rv) : "") + (s.rg ? ",\"rg\":" + std::to_string(s.rg) : "")).c_str()));
        }
      }
      std::vector<std::string> out; { std::lock_guard<std::mutex> l(sh_.mu); out.swap(sh_.outbox); }
      if (v2_) for (const std::string& s : out) sendRaw(s);
      if (now - lastTx_ >= 1000) sendRaw("{\"t\":\"ping\"}");
    }
    // ---- drive the game-thread tick (Android: trainer command -> engine hook)
    if (env_.requestTick) {
      if (env_.epoch && now - lastEpochPoll > 250) { lastEpochPoll = now; int e = env_.epoch(); if (e != epoch_) { if (epoch_ != 0) { logf("scene changed (epoch %d -> %d): respawning remotes", epoch_.load(), e); epochChanged_ = true; } epoch_ = e; } }
      uint64_t done = env_.ticksDone ? env_.ticksDone() : 0; uint64_t inflight = tickReqCount_ > done ? tickReqCount_ - done : 0;
      if (inflight == 0) waitSince_ = 0; else if (waitSince_ == 0) waitSince_ = now;
      if (done == 0 && inflight > 0 && waitSince_ > 0 && now - waitSince_ > 3000 && !tickWarned_) { logf("tick commands are not reaching the hook; world sync is inactive"); tickWarned_ = true; }
      bool any = stats.puppetPlayers > 0 || stats.puppetEnts > 0; double interval = any ? 16 : 33;
      if (joined_ && inflight <= 1 && now - lastTickReq_ >= interval && (!env_.tickReady || env_.tickReady())) { if (env_.requestTick()) tickReqCount_++; lastTickReq_ = now; }
      if (inflight > 1 && waitSince_ > 0 && now - waitSince_ > 1500) { tickReqCount_ = done; waitSince_ = 0; } // lost command: resync the counter
    }
    static double lastSay = 0; if (now - lastSay > 400) { lastSay = now; sayFile(); }
    writeStatus(false);
    sleep_ms(4);
  }
  if (sock_ >= 0) { sendRaw("{\"t\":\"leave\"}"); close(sock_); sock_ = -1; }
}
}  // namespace mp
