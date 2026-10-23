'use strict';
// GTAV MP relay server v2: UDP state relay + shared world (clock, weather, NPCs, vehicles) + HTTP mod distribution.
// Usage: node server.js [server.json]      Console commands: type "help" (or /help in chat as the host).
// Protocol: see PROTOCOL.md. v1 clients (position/weapon/vehicle only) keep working unchanged.
const dgram = require('dgram'), http = require('http'), fs = require('fs'), path = require('path'), crypto = require('crypto'), os = require('os');
const { execSync } = require('child_process');

const ROOT = __dirname;
const cfgPath = process.argv[2] || path.join(ROOT, 'server.json');
const DEFAULTS = {
  hosts: 'auto', port: 30120, httpPort: 30121, tickHz: 10, maxPlayers: 16, timeoutMs: 10000, requireMods: true, motd: 'GTAV MP', modsDir: 'mods',
  world: { hour: 22, minute: 0, timeScale: 30, frozen: false, weather: 'CLEAR' },   // timeScale: game seconds per real second (30 = GTA default, 2 s per game minute)
  entities: { interestRadius: 300, maxPerPlayer: 150, maxSend: 80, expireMs: 2500, rateHz: 6 },
  population: 'host',   // 'host': only the host's game generates NPC traffic, everyone else mirrors it
};
const user = fs.existsSync(cfgPath) ? JSON.parse(fs.readFileSync(cfgPath, 'utf8')) : {};
const cfg = Object.assign({}, DEFAULTS, user, { world: Object.assign({}, DEFAULTS.world, user.world), entities: Object.assign({}, DEFAULTS.entities, user.entities) });
const modsDir = path.resolve(ROOT, cfg.modsDir);
const PROTO_MIN = 1, PROTO_MAX = 2, MAX_DGRAM = 1400;
const MOD_EXT = new Set(['.zip', '.rar', '.oiv']);
const WEATHERS = new Set(['CLEAR', 'EXTRASUNNY', 'CLOUDS', 'OVERCAST', 'RAIN', 'CLEARING', 'THUNDER', 'SMOG', 'FOGGY', 'XMAS', 'SNOWLIGHT', 'BLIZZARD', 'SNOW', 'NEUTRAL']);

function log(s) { console.log(new Date().toISOString().slice(11, 19), s); }
const num = (v, lo, hi) => (typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi);
const int = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
const arrOfInts = (a, maxLen, lo, hi) => Array.isArray(a) && a.length <= maxLen && a.every(x => int(x, lo, hi));
const SAFE_ID = /^[A-Za-z0-9_@ .-]{0,48}$/;

// ---------- mod manifest ----------
function sha256File(p) { return crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex'); }
function buildManifest() {
  fs.mkdirSync(modsDir, { recursive: true });
  const metaPath = path.join(modsDir, 'mods.json');
  const meta = fs.existsSync(metaPath) ? JSON.parse(fs.readFileSync(metaPath, 'utf8')) : {};
  const mods = [];
  for (const name of fs.readdirSync(modsDir).sort()) {
    if (!MOD_EXT.has(path.extname(name).toLowerCase())) continue;
    const full = path.join(modsDir, name), m = meta[name] || {};
    const kind = ['replace', 'addon', 'script'].includes(m.kind) ? m.kind : 'addon';
    mods.push({ name, size: fs.statSync(full).size, sha256: sha256File(full), kind, required: kind !== 'addon' ? true : !!m.required });
  }
  const setHash = crypto.createHash('sha256').update(mods.filter(m => m.required).map(m => `${m.name}:${m.sha256}`).join('\n')).digest('hex');
  return { motd: cfg.motd, proto: PROTO_MAX, setHash, mods };
}
let manifest = buildManifest();
const modFiles = () => new Map(manifest.mods.map(m => [m.name, m]));
const httpServer = http.createServer((req, res) => {
  const url = decodeURIComponent((req.url || '/').split('?')[0]);
  if (url === '/manifest.json') { res.writeHead(200, { 'Content-Type': 'application/json' }); return res.end(JSON.stringify(manifest)); }
  const m = /^\/mods\/([^/]+)$/.exec(url), entry = m && modFiles().get(m[1]);
  if (!entry) { res.writeHead(404); return res.end('not found'); }
  res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': entry.size });
  fs.createReadStream(path.join(modsDir, entry.name)).pipe(res);
});
httpServer.listen(cfg.httpPort, () => log(`http :${cfg.httpPort} (${manifest.mods.length} mods, setHash ${manifest.setHash.slice(0, 12)})`));

// ---------- shared world state ----------
const world = { sec: (cfg.world.hour * 3600 + cfg.world.minute * 60) % 86400, scale: cfg.world.timeScale, frozen: !!cfg.world.frozen, weather: WEATHERS.has(String(cfg.world.weather).toUpperCase()) ? String(cfg.world.weather).toUpperCase() : 'CLEAR' };
let lastWorldTick = Date.now();
const worldMsg = () => ({ t: 'wt', s: Math.floor(world.sec), sc: world.scale, fz: world.frozen ? 1 : 0, w: world.weather });
function setWorld(patch, who) {
  Object.assign(world, patch);
  log(`world: ${String(Math.floor(world.sec / 3600)).padStart(2, '0')}:${String(Math.floor(world.sec / 60) % 60).padStart(2, '0')} ${world.weather} x${world.scale}${world.frozen ? ' frozen' : ''}${who ? ' (by ' + who + ')' : ''}`);
  broadcastV2(worldMsg());
}

// ---------- players / entities ----------
const socks = new Map();           // local address -> udp socket
const players = new Map();         // "ip:port" -> player
const byId = new Map();            // id -> player
const xferred = new Map();         // gid -> expiry: entities a player took over; late rows from the old controller are ignored
const ents = new Map();            // gid -> { g,k,m,x,y,z,h,vx,vy,vz,fl,a,b, ctl, last, raw }
let nextId = 1, hostId = 0;

function send(rinfo, obj) {
  const b = Buffer.from(JSON.stringify(obj));
  if (b.length > MAX_DGRAM) { log(`warn: dropping oversize ${obj.t} packet (${b.length} B)`); return; }
  rinfo.sock.send(b, rinfo.port, rinfo.address, err => { if (err) log(`send error to ${rinfo.address}:${rinfo.port}: ${err.message}`); });
}
function broadcast(obj, exceptId) { for (const p of players.values()) if (p.id !== exceptId) send(p.rinfo, obj); }
function broadcastV2(obj, exceptId) { for (const p of players.values()) if (p.v >= 2 && p.id !== exceptId) send(p.rinfo, obj); }
function cleanName(n) { return String(n || '').replace(/[^A-Za-z0-9 _.-]/g, '').trim().slice(0, 24); }
function pickHost() { let best = 0; for (const p of players.values()) if (p.v >= 2 && (!best || p.id < best)) best = p.id; return best; }
function announceHost() { const h = pickHost(); if (h !== hostId) { hostId = h; log(`world host: ${h ? '#' + h + ' ' + byId.get(h).name : 'none'}`); broadcastV2({ t: 'host', h }); } }

function onMessage(buf, rinfo0, sock) {
  if (buf.length > MAX_DGRAM) return;
  const rinfo = { address: rinfo0.address, port: rinfo0.port, sock };
  let msg; try { msg = JSON.parse(buf.toString('utf8')); } catch { return; }
  if (!msg || typeof msg.t !== 'string') return;
  const key = `${rinfo.address}:${rinfo.port}`; let p = players.get(key); const now = Date.now();

  if (msg.t === 'join') {
    if (p) { // retry from a client that has not received our "ok" yet: keep it alive and answer again
      p.last = now; p.rx.join++;
      if (p.rx.join === 3) log(`! ${p.name} #${p.id} keeps re-sending join but never sends ping/state: it is not receiving our replies (UDP return path). rx=${JSON.stringify(p.rx)}`);
      return send(rinfo, okMsg(p));
    }
    const name = cleanName(msg.name);
    if (!int(msg.v, PROTO_MIN, PROTO_MAX)) return send(rinfo, { t: 'deny', r: 'protocol version mismatch' });
    if (!name) return send(rinfo, { t: 'deny', r: 'invalid name' });
    for (const [k, o] of players) if (o.name.toLowerCase() === name.toLowerCase() && o.rinfo.address === rinfo.address) drop(k, 'reconnected');
    if (players.size >= cfg.maxPlayers) return send(rinfo, { t: 'deny', r: 'server full' });
    for (const o of players.values()) if (o.name.toLowerCase() === name.toLowerCase()) return send(rinfo, { t: 'deny', r: 'name in use' });
    if (cfg.requireMods && msg.mods !== manifest.setHash) {
      log(`x ${name} (${key}) denied: mod set mismatch (client ${String(msg.mods).slice(0, 12)} vs server ${manifest.setHash.slice(0, 12)})`);
      return send(rinfo, { t: 'deny', r: 'mod set mismatch', http: cfg.httpPort, setHash: manifest.setHash });
    }
    p = { id: nextId++, name, rinfo, v: msg.v, st: null, app: null, wep: null, last: now, lastMsg: 0, joinedAt: now, rx: { join: 1, ping: 0, s: 0, a: 0, wp: 0, an: 0, e: 0, ed: 0, xfer: 0, chat: 0, other: 0 } };
    players.set(key, p); byId.set(p.id, p);
    log(`+ ${name} #${p.id} v${p.v} (${key})`);
    send(rinfo, okMsg(p));
    broadcast({ t: 'chat', n: '*', m: `${name} joined` }, p.id);
    if (p.v >= 2) {
      send(rinfo, worldMsg());
      for (const o of players.values()) { // late join snapshot: everybody's look and loadout
        if (o.id === p.id || o.v < 2) continue;
        if (o.app) send(rinfo, Object.assign({ t: 'a', i: o.id }, o.app));
        if (o.wep) send(rinfo, Object.assign({ t: 'wp', i: o.id }, o.wep));
      }
      announceHost();
    }
    return;
  }
  if (!p) return;
  p.last = now;
  Object.hasOwn(p.rx, msg.t) ? p.rx[msg.t]++ : p.rx.other++;

  switch (msg.t) {
    case 's': {
      if (now - p.lastMsg < 20) return; p.lastMsg = now;
      if (!(num(msg.x, -20000, 20000) && num(msg.y, -20000, 20000) && num(msg.z, -2000, 5000) && num(msg.h, -720, 720))) return;
      p.st = {
        x: msg.x, y: msg.y, z: msg.z, h: msg.h, hp: num(msg.hp, 0, 1000) ? msg.hp : 200,
        vx: num(msg.vx, -200, 200) ? msg.vx : 0, vy: num(msg.vy, -200, 200) ? msg.vy : 0, vz: num(msg.vz, -200, 200) ? msg.vz : 0,
        wp: Number.isInteger(msg.wp) && msg.wp >= 0 && msg.wp <= 0xFFFFFFFF ? msg.wp : 0, iv: msg.iv ? 1 : 0,
        vm: typeof msg.vm === 'string' && /^[A-Za-z0-9_]{0,32}$/.test(msg.vm) ? msg.vm : '',
        // v2 extras: fl = bit flags (1 aim, 2 shoot, 4 reload, 8 crouch, 16 ragdoll, 32 sprint, 64 in vehicle, 128 parachute, 256 swim, 512 climb, 1024 cover, 2048 dead), sd = seat, am = ammo in clip
        fl: int(msg.fl, 0, 0xFFFFF) ? msg.fl : 0, sd: int(msg.sd, -2, 16) ? msg.sd : -2, am: int(msg.am, 0, 99999) ? msg.am : 0,
      };
      break;
    }
    case 'a': { // appearance: model (hash or name), c = 12 x [drawable,texture,palette], p = 8 x [index,texture], hb = head blend, ht = hair tints
      if (p.v < 2) return;
      const m = msg.m, ok = (int(m, 0, 0xFFFFFFFF) || (typeof m === 'string' && /^[A-Za-z0-9_]{1,32}$/.test(m)));
      if (!ok || !arrOfInts(msg.c, 36, -1, 65535) || !arrOfInts(msg.p || [], 16, -1, 65535)) return;
      const app = { m, c: msg.c, p: msg.p || [] };
      if (arrOfInts(msg.hb, 12, 0, 255)) app.hb = msg.hb;
      if (arrOfInts(msg.ht, 4, 0, 255)) app.ht = msg.ht;
      p.app = app; broadcastV2(Object.assign({ t: 'a', i: p.id }, app), p.id); break;
    }
    case 'wp': { // loadout: w = current weapon hash, am = ammo, c = component hashes of current weapon, all = [[hash, ammo], ...]
      if (p.v < 2) return;
      if (!int(msg.w, 0, 0xFFFFFFFF) || !int(msg.am || 0, 0, 99999) || !arrOfInts(msg.c || [], 12, 0, 0xFFFFFFFF)) return;
      const wep = { w: msg.w, am: msg.am || 0, c: msg.c || [] };
      if (Array.isArray(msg.all) && msg.all.length <= 24 && msg.all.every(x => Array.isArray(x) && x.length === 2 && int(x[0], 0, 0xFFFFFFFF) && int(x[1], 0, 99999))) wep.all = msg.all;
      p.wep = wep; broadcastV2(Object.assign({ t: 'wp', i: p.id }, wep), p.id); break;
    }
    case 'an': { // one-shot animation: d = dict, c = clip, f = flags, s = 1 start / 0 stop
      if (p.v < 2) return;
      if (!SAFE_ID.test(String(msg.d || '')) || !SAFE_ID.test(String(msg.c || '')) || !int(msg.f || 0, 0, 0xFFFFFF)) return;
      broadcastV2({ t: 'an', i: p.id, d: msg.d || '', c: msg.c || '', f: msg.f || 0, s: msg.s ? 1 : 0 }, p.id); break;
    }
    case 'e': { // entity batch from its controller: e = [[gid,kind,model,x,y,z,h,vx,vy,vz,fl,a,b], ...]  kind 1 ped, 2 vehicle
      if (p.v < 2 || !Array.isArray(msg.e) || msg.e.length > 60) return;
      let mine = 0; for (const e of ents.values()) if (e.ctl === p.id) mine++;
      for (const r of msg.e) {
        if (!Array.isArray(r) || r.length < 10 || r.length > 13) continue;
        const [g, k, m, x, y, z, h, vx, vy, vz, fl = 0, a = 0, b = 0] = r;
        if (!(int(g, 1, 0x7FFFFFFF) && int(k, 1, 2) && int(m, 0, 0xFFFFFFFF) && num(x, -20000, 20000) && num(y, -20000, 20000) && num(z, -2000, 5000) && num(h, -720, 720)
          && num(vx, -300, 300) && num(vy, -300, 300) && num(vz, -300, 300) && int(fl, 0, 0xFFFFFFF) && int(a, -2147483648, 2147483647) && int(b, -2147483648, 2147483647))) continue;
        let e = ents.get(g);
        if (!e && xferred.has(g) && xferred.get(g) > now) continue;
        if (!e) { if ((g >>> 16) !== p.id || mine >= cfg.entities.maxPerPlayer) continue; e = { g, ctl: p.id }; ents.set(g, e); mine++; }
        else if (e.ctl !== p.id) continue;
        Object.assign(e, { k, m, x, y, z, h, vx, vy, vz, fl, a, b, last: now });
      }
      break;
    }
    case 'ed': { // controller deletes its entities: l = [gid, ...]
      if (p.v < 2 || !arrOfInts(msg.l, 60, 1, 0x7FFFFFFF)) return;
      const gone = msg.l.filter(g => { const e = ents.get(g); return e && e.ctl === p.id && ents.delete(g); });
      if (gone.length) broadcastV2({ t: 'ed', l: gone }, p.id); break;
    }
    case 'xfer': { // take control of an entity (e.g. the player entered an NPC's car): the old controller becomes a puppet
      if (p.v < 2 || !int(msg.g, 1, 0x7FFFFFFF)) return;
      const e = ents.get(msg.g); if (!e || e.ctl === p.id) return;
      // the player now drives it, so it is represented by the player's own state (puppet car): remove the NPC copy everywhere
      ents.delete(e.g); xferred.set(e.g, now + 15000);
      const old = byId.get(e.ctl);
      if (old) send(old.rinfo, { t: 'take', g: e.g });
      broadcastV2({ t: 'ed', l: [e.g] }, p.id); log(`${p.name} #${p.id} took over entity ${e.g} from #${e.ctl}`); break;
    }
    case 'chat': {
      const text = String(msg.m || '').replace(/[^\x20-\x7E]/g, '').slice(0, 120);
      if (!text) break;
      if (text[0] === '/') { runCommand(text.slice(1), p); break; }
      log(`<${p.name}> ${text}`); broadcast({ t: 'chat', n: p.name, m: text }); break;
    }
    case 'leave': drop(key, 'left'); break;
    default: break; // 'ping' and anything unknown only refresh the keepalive
  }
}

function okMsg(p) {
  const m = { t: 'ok', id: p.id, tick: cfg.tickHz, http: cfg.httpPort, setHash: manifest.setHash };
  if (p.v >= 2) Object.assign(m, { v: 2, host: hostId || p.id, pop: cfg.population, er: cfg.entities.rateHz, ir: cfg.entities.interestRadius });
  return m;
}
function drop(key, why) {
  const p = players.get(key); if (!p) return;
  players.delete(key); byId.delete(p.id);
  const gone = []; for (const [g, e] of ents) if (e.ctl === p.id) { ents.delete(g); gone.push(g); }
  for (let i = 0; i < gone.length; i += 60) broadcastV2({ t: 'ed', l: gone.slice(i, i + 60) });
  log(`- ${p.name} #${p.id} (${why}) after ${((Date.now() - p.joinedAt) / 1000).toFixed(1)}s rx=${JSON.stringify(p.rx)}`);
  broadcast({ t: 'left', i: p.id }); broadcast({ t: 'chat', n: '*', m: `${p.name} ${why}` });
  announceHost();
}

// ---------- admin commands (console or host via chat) ----------
function runCommand(line, who) {
  const isConsole = !who, name = who ? who.name : 'console';
  const reply = s => { if (who) send(who.rinfo, { t: 'chat', n: '*', m: s.slice(0, 120) }); else console.log(s); };
  if (who && who.id !== hostId) return reply('only the host can use commands');
  const [cmd, ...a] = line.trim().split(/\s+/); const c = (cmd || '').toLowerCase();
  if (c === 'time') {
    const h = parseInt(a[0], 10), m = parseInt(a[1] || '0', 10);
    if (!int(h, 0, 23) || !int(m, 0, 59)) return reply('usage: time <hour 0-23> [minute]');
    setWorld({ sec: h * 3600 + m * 60 }, name);
  } else if (c === 'weather') {
    const w = String(a[0] || '').toUpperCase(); if (!WEATHERS.has(w)) return reply('weather: ' + [...WEATHERS].join(' '));
    setWorld({ weather: w }, name);
  } else if (c === 'freeze') { setWorld({ frozen: !world.frozen }, name);
  } else if (c === 'speed') { const s = parseFloat(a[0]); if (!(s >= 0 && s <= 600)) return reply('usage: speed <game seconds per real second, 30 = normal>'); setWorld({ scale: s }, name);
  } else if (c === 'players') { reply([...players.values()].map(p => `#${p.id} ${p.name} v${p.v}${p.id === hostId ? ' (host)' : ''}`).join(', ') || 'nobody');
  } else if (c === 'stats') { reply(`players ${players.size}, entities ${ents.size}`);
  } else if (c === 'say' && isConsole) { broadcast({ t: 'chat', n: 'server', m: a.join(' ').slice(0, 120) });
  } else reply('commands: time <h> [m] | weather <NAME> | freeze | speed <x> | players | stats' + (isConsole ? ' | say <text>' : ''));
}
if (process.stdin.isTTY) require('readline').createInterface({ input: process.stdin }).on('line', l => { if (l.trim()) runCommand(l); });

// ---------- tick loops ----------
setInterval(() => { // players + keepalive timeouts (v1 format, v2 receivers also get extras)
  const now = Date.now();
  for (const [key, p] of players) if (now - p.last > cfg.timeoutMs) drop(key, 'timed out');
  const all = [...players.values()].filter(p => p.st);
  for (const p of players.values()) {
    const others = all.filter(o => o.id !== p.id).map(o => {
      const r = { i: o.id, n: o.name, x: +o.st.x.toFixed(2), y: +o.st.y.toFixed(2), z: +o.st.z.toFixed(2), h: +o.st.h.toFixed(1), hp: o.st.hp,
        vx: +o.st.vx.toFixed(2), vy: +o.st.vy.toFixed(2), vz: +o.st.vz.toFixed(2), wp: o.st.wp, iv: o.st.iv, vm: o.st.vm };
      if (p.v >= 2) { r.fl = o.st.fl; r.sd = o.st.sd; r.am = o.st.am; }
      return r;
    });
    send(p.rinfo, { t: 'w', p: others });
  }
}, Math.round(1000 / cfg.tickHz));

setInterval(() => { // world clock
  const now = Date.now(), dt = (now - lastWorldTick) / 1000; lastWorldTick = now;
  if (!world.frozen) world.sec = (world.sec + dt * world.scale) % 86400;
}, 250);
setInterval(() => { if (players.size) broadcastV2(worldMsg()); }, 2000);

setInterval(() => { // entity relay with interest management + expiry
  const now = Date.now(), R2 = cfg.entities.interestRadius ** 2, expired = [];
  for (const [g, e] of ents) if (now - e.last > cfg.entities.expireMs) { ents.delete(g); expired.push(g); }
  for (let i = 0; i < expired.length; i += 60) broadcastV2({ t: 'ed', l: expired.slice(i, i + 60) });
  for (const p of players.values()) {
    if (p.v < 2 || !p.st) continue;
    const near = [];
    for (const e of ents.values()) {
      if (e.ctl === p.id) continue;
      const dx = e.x - p.st.x, dy = e.y - p.st.y, d2 = dx * dx + dy * dy;
      if (d2 <= R2) near.push([d2, e]);
    }
    near.sort((a, b) => a[0] - b[0]);
    let chunk = [], size = 20;
    const flush = () => { if (chunk.length) send(p.rinfo, { t: 'e', e: chunk }); chunk = []; size = 20; };
    for (const [, e] of near.slice(0, cfg.entities.maxSend)) {
      const r = [e.g, e.k, e.m, +e.x.toFixed(2), +e.y.toFixed(2), +e.z.toFixed(2), +e.h.toFixed(1), +e.vx.toFixed(1), +e.vy.toFixed(1), +e.vz.toFixed(1), e.fl, e.a, e.b, e.ctl];
      const len = JSON.stringify(r).length + 1;
      if (size + len > 1200) flush();
      chunk.push(r); size += len;
    }
    flush();
  }
}, Math.round(1000 / cfg.entities.rateHz));

// ---------- automatic multi-address UDP ----------
// One UDP socket per local IPv4 address so every client is answered from the exact address it sent to
// (a wildcard socket on a phone with hotspot + mobile data + wifi can answer from the wrong interface). Rescanned every 5 s.
function detectAddrs() {
  const out = new Set(['127.0.0.1']);
  try { for (const list of Object.values(os.networkInterfaces())) for (const a of list || []) if (a.family === 'IPv4' || a.family === 4) out.add(a.address); } catch (e) { /* Termux can deny this */ }
  if (out.size <= 1) for (const cmd of ['ifconfig', 'ip -4 -o addr show']) {
    try { const txt = execSync(cmd + ' 2>/dev/null', { encoding: 'utf8', timeout: 3000 }); for (const m of txt.matchAll(/inet (?:addr:)?(\d+\.\d+\.\d+\.\d+)/g)) out.add(m[1]); if (out.size > 1) break; } catch (e) { /* next */ }
  }
  return out;
}
const fixedHosts = Array.isArray(cfg.hosts) ? cfg.hosts : (cfg.hosts && cfg.hosts !== 'auto' ? [cfg.hosts] : []);
function openAddr(h) {
  const s = dgram.createSocket('udp4'); socks.set(h, s);
  s.on('message', (b, r) => onMessage(b, r, s));
  s.on('error', e => { log(`udp ${h}: ${e.message}`); socks.delete(h); try { s.close(); } catch (x) {} });
  s.bind(cfg.port, h, () => log(`udp ${h}:${cfg.port} tick ${cfg.tickHz}Hz max ${cfg.maxPlayers}` + (h === '127.0.0.1' ? '  (this device)' : '  <- other phones use server=' + h)));
}
function rescan() {
  const want = detectAddrs(); for (const h of fixedHosts) want.add(h);
  for (const h of want) if (!socks.has(h)) openAddr(h);
  for (const [h, s] of socks) if (!want.has(h)) { log(`address ${h} is gone, closing`); socks.delete(h); try { s.close(); } catch (e) {} }
}
rescan(); setInterval(rescan, 5000);
setTimeout(() => { if (!socks.size) { log('no UDP socket could bind'); process.exit(1); } }, 3000);
log(`world: ${String(Math.floor(world.sec / 3600)).padStart(2, '0')}:${String(Math.floor(world.sec / 60) % 60).padStart(2, '0')} ${world.weather} x${world.scale}, population mode: ${cfg.population}`);
process.on('SIGHUP', () => { manifest = buildManifest(); log('manifest reloaded'); });
