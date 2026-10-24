// Test bot v2: a fake second player so you can check EVERYTHING alone.
//   node test_bot.js [host] [udpPort] [httpPort]
// Control it by typing in the game chat (mp_say.txt):  !bot help
const dgram = require('dgram'), http = require('http');
const host = process.argv[2] || '127.0.0.1', port = +process.argv[3] || 30120, hport = +process.argv[4] || 30121;
const joaat = s => { let h = 0; for (const c of s.toLowerCase()) { h = (h + c.charCodeAt(0)) >>> 0; h = (h + (h << 10)) >>> 0; h = (h ^ (h >>> 6)) >>> 0; } h = (h + (h << 3)) >>> 0; h = (h ^ (h >>> 11)) >>> 0; h = (h + (h << 15)) >>> 0; return h >>> 0; };
const PISTOL = joaat('WEAPON_PISTOL'), RPG = joaat('WEAPON_RPG');
const FL = { AIM: 1, SHOOT: 2, RELOAD: 4, CROUCH: 8, RAGDOLL: 16, SPRINT: 32, VEH: 64, SWIM: 256, DEAD: 2048 };
const flNames = fl => Object.entries(FL).filter(([, v]) => fl & v).map(([k]) => k).join('+') || '-';
const vdText = vd => { if (!vd) return 'no car detail'; const b = vd[5], x = vd[4];
  const doors = [...Array(8).keys()].filter(i => b >> (8 + i) & 1), tyres = [...Array(6).keys()].filter(i => b >> (16 + i) & 1);
  return `engine ${b & 1} lights ${b >> 1 & 1} beam ${b >> 2 & 1} siren ${b >> 3 & 1} horn ${b >> 4 & 1} doors[${doors}] tyres[${tyres}] body ${vd[6]} eng ${vd[7]} ${x & 1 ? 'EXPLODED ' : ''}${x & 2 ? 'FIRE' : ''}`; };

const st = { mode: 'walk', model: 'sultan', wp: 0, fl: 0, hp: 200, car: { engine: 1, lights: 0, beam: 0, siren: 0, horn: 0, doors: 0, tyres: 0, boom: 0, fire: 0 }, npcs: false, aimAtMe: false };
let myId = 0, me = null, seenE = [], lastLog = '', t0 = Date.now();
const s = dgram.createSocket('udp4'), send = o => s.send(Buffer.from(JSON.stringify(o)), port, host);
const say = m => { send({ t: 'chat', m: String(m).slice(0, 118) }); console.log('say:', m); };

function report() {
  if (!me) return say('I do not see you yet (move a little / wait 2 s)');
  say(`you: hp ${me.hp} fl ${flNames(me.fl || 0)} wp ${me.wp} cam ${me.cp ?? '?'}/${me.cy ?? '?'} veh ${me.vm || '-'}${me.rv ? ' passenger of #' + me.rv : ''}${me.rg ? ' passenger of npc car ' + me.rg : ''}`);
  if (me.vm) say('your car: ' + vdText(me.vd));
  const peds = seenE.filter(r => r[1] === 1), cars = seenE.filter(r => r[1] === 2);
  say(`you stream ${peds.length} peds (${peds.filter(r => r[14]).length} with clothes), ${cars.length} cars (${cars.filter(r => r[14]).length} with detail)`);
}
function help() { say('!bot walk|stand|car|ride|gun|shoot|shootme|die|revive'); say('!bot lights beam siren horn door tire explode fire (car)'); say('!bot npcs [off] | wanted N | say TEXT | report'); }

function cmd(line) {
  const [c, ...a] = line.trim().split(/\s+/);
  switch ((c || '').toLowerCase()) {
    case 'help': return help();
    case 'ride': st.mode = 'ride'; break;   // bot sits as passenger in YOUR car
    case 'walk': case 'stand': st.mode = c; st.fl &= ~FL.VEH; break;
    case 'car': st.mode = 'car'; if (a[0]) st.model = a[0]; break;
    case 'gun': st.wp = st.wp ? 0 : PISTOL; break;
    case 'rpg': st.wp = RPG; break;
    case 'shoot': st.wp = st.wp || PISTOL; st.fl ^= FL.AIM | FL.SHOOT; st.aimAtMe = false; break;
    case 'shootme': st.wp = st.wp || PISTOL; st.fl |= FL.AIM | FL.SHOOT; st.aimAtMe = true; break;
    case 'die': st.fl |= FL.DEAD; st.hp = 0; break;
    case 'revive': st.fl &= ~FL.DEAD; st.hp = 200; break;
    case 'lights': st.car.lights ^= 1; break; case 'beam': st.car.beam ^= 1; break; case 'siren': st.car.siren ^= 1; break;
    case 'horn': st.car.horn = 1; setTimeout(() => st.car.horn = 0, 800); break;
    case 'door': st.car.doors ^= 1; break; case 'tire': st.car.tyres ^= 1; break;
    case 'explode': st.car.boom = 1; break; case 'fire': st.car.fire ^= 1; break;
    case 'npcs': st.npcs = a[0] !== 'off'; break;
    case 'wanted': send({ t: 'wl', l: Math.max(0, Math.min(5, +a[0] || 0)) }); break;
    case 'say': return say(a.join(' '));
    case 'report': return report();
    default: return help();
  }
  console.log('state', JSON.stringify(st));
}

const vdOf = (x, y, h) => { const r = h * Math.PI / 180 / 2, c = st.car;
  const bits = (c.engine ? 1 : 0) | (c.lights ? 2 : 0) | (c.beam ? 4 : 0) | (c.siren ? 8 : 0) | (c.horn ? 16 : 0) | (c.doors << 8) | (c.tyres << 16);
  return [0, 0, +Math.sin(r).toFixed(4), +Math.cos(r).toFixed(4), (c.boom ? 1 : 0) | (c.fire ? 2 : 0), bits, c.boom ? 0 : 1000, c.boom ? 0 : 1000]; };

function tickState() {
  if (!me || !myId) return;
  const t = (Date.now() - t0) / 1000; let x = me.x + 6, y = me.y, z = me.z, vx = 0, vy = 0, h = 0, vm = '', fl = st.fl, sd = -2, vd;
  if (st.mode === 'walk') { const a = t * 0.5, R = 6; x = me.x + R * Math.cos(a); y = me.y + R * Math.sin(a); vx = -R * 0.5 * Math.sin(a); vy = R * 0.5 * Math.cos(a); h = ((Math.atan2(vy, vx) * 180 / Math.PI) + 270) % 360; fl |= FL.SPRINT * 0; }
  if (st.mode === 'ride') { if (!me.vm) return; x = me.x; y = me.y; z = me.z; vx = me.vx; vy = me.vy; h = me.h; vm = me.vm; fl |= FL.VEH; sd = 0; }
  if (st.mode === 'car') { const a = t * 0.35, R = 18; x = me.x + R * Math.cos(a); y = me.y + R * Math.sin(a); vx = -R * 0.35 * Math.sin(a); vy = R * 0.35 * Math.cos(a); h = ((Math.atan2(vy, vx) * 180 / Math.PI) + 270) % 360;
    vm = String(joaat(st.model)); fl |= FL.VEH; sd = -1; vd = vdOf(x, y, h); }
  let cp = 0, cy = 0;
  if (fl & FL.AIM) { const dx = (st.aimAtMe ? me.x : x + 30) - x, dy = (st.aimAtMe ? me.y : y) - y, dz = (st.aimAtMe ? me.z + 0.5 : z) - z;
    cy = Math.atan2(-dx, dy) * 180 / Math.PI; cp = Math.atan2(dz, Math.hypot(dx, dy)) * 180 / Math.PI; h = cy < 0 ? cy + 360 : cy; }
  const o = { t: 's', x, y, z, h, hp: st.hp, vx, vy, vz: 0, wp: st.wp, iv: 0, vm, fl, sd, am: 30, cp: +cp.toFixed(1), cy: +cy.toFixed(1) };
  if (vd) o.vd = vd; if (st.mode === 'ride' && me.vm) o.rv = me.i; send(o);
}

function tickNpcs() {   // the bot is the owner of a small test population around you
  if (!st.npcs || !me || !myId) return; const t = (Date.now() - t0) / 1000, base = myId << 16, rows = [];
  const clothes = Array.from({ length: 24 }, (_, i) => i % 2 ? 0 : (i / 2 + 1) % 5);
  rows.push([base + 1, 1, joaat('a_m_y_hipster_01'), me.x - 8 + 3 * Math.cos(t), me.y + 8 + 3 * Math.sin(t), me.z, 0, -Math.sin(t), Math.cos(t), 0, 0, 200, 0, clothes]);            // walker with its own clothes
  rows.push([base + 2, 1, joaat('s_m_y_cop_01'), me.x - 12, me.y + 4, me.z, 90, 0, 0, 0, FL.AIM | FL.SHOOT, 200, PISTOL, clothes]);                                                  // cop shooting
  rows.push([base + 3, 1, joaat('a_f_y_beach_01'), me.x - 12, me.y + 8, me.z, 0, 0, 0, 0, FL.DEAD, 0, 0, clothes]);                                                                 // dead body
  rows.push([base + 4, 1, joaat('a_m_m_business_01'), me.x - 12, me.y + 12, me.z, 0, 0, 0, 0, FL.RAGDOLL, 200, 0]);                                                                 // ragdoll
  const car = (n, dx, c) => { const prev = Object.assign({}, st.car); Object.assign(st.car, c); const vd = vdOf(0, 0, 0); Object.assign(st.car, prev); rows.push([base + n, 2, joaat('sultan'), me.x + dx, me.y + 14, me.z, 0, 0, 0, 0, 0, 0, 0, vd]); };
  car(5, -10, { lights: 1, beam: 1 });                    // lights + high beam
  car(6, -6, { doors: 5, tyres: 3, lights: 0 });          // doors 0 and 2 open, tyres 0 and 1 burst
  car(7, -2, { siren: 1, lights: 1 });                    // siren
  car(8, 2, { boom: 1, fire: 1 });                        // wreck, burning
  send({ t: 'e', e: rows });
}

http.get({ host, port: hport, path: '/manifest.json' }, r => {
  let b = ''; r.on('data', d => b += d); r.on('end', () => {
    const man = JSON.parse(b);
    s.on('message', m => { let j; try { j = JSON.parse(m); } catch (e) { return; }
      if (j.t === 'ok') { myId = j.id; console.log('bot joined as #' + myId); say('bot ready: type !bot help'); }
      else if (j.t === 'w') { const p = j.p.find(p => p.i !== myId); if (p) { me = p; const txt = `fl ${flNames(p.fl || 0)} wp ${p.wp} veh ${p.vm || '-'} ${p.vm ? vdText(p.vd) : ''}`; if (txt !== lastLog) { lastLog = txt; console.log('YOU:', txt); } } }
      else if (j.t === 'e') { for (const r of j.e) { const i = seenE.findIndex(x => x[0] === r[0]); if (i >= 0) seenE[i] = r; else seenE.push(r); } if (seenE.length > 300) seenE.shift(); }
      else if (j.t === 'chat' && j.n !== 'Bot' && /^!bot\b/.test(j.m)) cmd(j.m.replace(/^!bot\s*/, ''));
      else if (j.t === 'wl') console.log('wanted level now', j.l);
    });
    send({ t: 'join', name: 'Bot', v: 2, mods: man.setHash });
    setInterval(() => send({ t: 'a', m: joaat('a_m_y_beach_01'), c: Array(36).fill(0), p: Array(16).fill(-1) }), 3000);   // looks
    setInterval(tickState, 100); setInterval(tickNpcs, 160);
    setInterval(() => { seenE = seenE.slice(-200); }, 30000);
    require('readline').createInterface({ input: process.stdin }).on('line', l => { if (l.trim()) cmd(l.replace(/^!bot\s*/, '')); });   // you can also type the commands right here
    console.log('bot joining', host + ':' + port, '- then type in game chat: !bot help');
  });
}).on('error', e => { console.log('cannot reach server:', e.message); process.exit(1); });
