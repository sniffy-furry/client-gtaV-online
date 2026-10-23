// Test bot: joins your server as "Bot" and walks in a circle around the first other player.
// Use it to check that remote players appear and move, without a friend.   node follow_bot.js [host] [udpPort] [httpPort]
const dgram = require('dgram'), http = require('http');
const host = process.argv[2] || '127.0.0.1', port = +process.argv[3] || 30120, hport = +process.argv[4] || 30121;
http.get({ host, port: hport, path: '/manifest.json' }, r => {
  let b = ''; r.on('data', d => b += d); r.on('end', () => {
    const man = JSON.parse(b), s = dgram.createSocket('udp4');
    let me = null, t0 = Date.now(), seen = 0;
    s.on('message', m => { try { const j = JSON.parse(m); if (j.t === 'w') me = j.p.find(p => p.n !== 'Bot') || me; } catch (e) {} });
    s.send(Buffer.from(JSON.stringify({ t: 'join', name: 'Bot', v: 1, mods: man.setHash })), port, host);
    console.log('bot joined', host + ':' + port, '- waiting for a player...');
    setInterval(() => {
      if (!me) return;
      const a = (Date.now() - t0) / 1000 * 0.5, R = 5;                  // 0.5 rad/s on a 5 m circle = 2.5 m/s
      const x = me.x + R * Math.cos(a), y = me.y + R * Math.sin(a), z = me.z;
      const vx = -R * 0.5 * Math.sin(a), vy = R * 0.5 * Math.cos(a);
      const h = ((Math.atan2(vy, vx) * 180 / Math.PI) + 270) % 360;     // face walking direction
      s.send(Buffer.from(JSON.stringify({ t: 's', x, y, z, h, hp: 200, vx, vy, vz: 0, wp: 0, iv: 0, vm: '' })), port, host);
      if (++seen % 50 === 1) console.log('following player at', me.x.toFixed(1), me.y.toFixed(1), me.z.toFixed(1));
    }, 100);
  });
}).on('error', e => { console.log('cannot reach server:', e.message); process.exit(1); });
