# GTAV MP protocol v2 (UDP, JSON, one datagram <= 1400 B)

v1 clients (join `"v":1`) keep working: they only get `ok`, `w`, `chat`, `left`. Everything below needs `"v":2` in `join`.

## Handshake
- C->S `{"t":"join","name":"Bob","v":2,"mods":"<setHash>"}` resent every 2 s until `ok`.
- S->C `{"t":"ok","id":3,"tick":10,"http":30121,"setHash":"..","v":2,"host":1,"pop":"host","er":6,"ir":300}`
  `host` = player id that runs the world population, `er` entity rate Hz, `ir` interest radius (m).
- C->S `{"t":"ping"}` every 1 s when nothing else is sent. 10 s of silence = dropped.

## Player state (C->S ~tick rate)
`{"t":"s","x","y","z","h","hp","vx","vy","vz","wp":<weapon hash>,"vm":"<vehicle model>","fl":<bits>,"sd":<seat -2..16>,"am":<ammo in clip>}`
`fl` bits: 1 aim, 2 shoot, 4 reload, 8 crouch, 16 ragdoll, 32 sprint, 64 in vehicle, 128 parachute, 256 swim, 512 climb, 1024 cover, 2048 dead.
S->C `{"t":"w","p":[{"i","n","x","y","z","h","hp","vx","vy","vz","wp","iv","vm","fl","sd","am"}]}` (fl/sd/am only for v2).

## Looks, loadout, animation (relayed on change; late joiners get a snapshot)
- `{"t":"a","m":<model hash|name>,"c":[12 x drawable,texture,palette],"p":[8 x propIndex,propTexture],"hb":[head blend],"ht":[hair tints]}` -> relayed as `{"t":"a","i":id,...}`
- `{"t":"wp","w":<hash>,"am":<ammo>,"c":[component hashes],"all":[[hash,ammo],...]}` -> `{"t":"wp","i":id,...}`
- `{"t":"an","d":"<dict>","c":"<clip>","f":<flags>,"s":1|0}` -> `{"t":"an","i":id,...}` (start/stop)

## World (server authoritative)
- S->C `{"t":"wt","s":<seconds of day>,"sc":<game s per real s>,"fz":0|1,"w":"RAIN"}` every 2 s and on change.
  Client: SET_CLOCK_TIME / NETWORK_OVERRIDE_CLOCK_TIME + PAUSE_CLOCK(true), advance locally by `sc`, correct drift when it is > 2 game minutes off; SET_WEATHER_TYPE_NOW_PERSIST for `w`.
- Host commands (console or chat as host): `/time 23 30`, `/weather RAIN`, `/freeze`, `/speed 30`, `/players`, `/stats`.
- S->C `{"t":"host","h":<id>}` when the host changes (lowest v2 id).

## NPCs and vehicles
Controller (owner) streams: `{"t":"e","e":[[gid,kind,model,x,y,z,h,vx,vy,vz,fl,a,b],...]}` (<= 60 per packet, kind 1 ped / 2 vehicle).
`gid = (playerId << 16) | localIndex` (only the creator may create it). Server relays to everyone else within `ir` metres, nearest first, as rows with the controller id appended: `[...,fl,a,b,ctl]`.
- Puppets: create on first sight, interpolate position/velocity, delete when `ed` arrives or no update for ~2 s.
- Delete: `{"t":"ed","l":[gid,...]}` (controller) -> relayed to all; server also sends `ed` on expiry (2.5 s) or when the controller leaves.
- Take over an entity (player enters an NPC car): C->S `{"t":"xfer","g":gid}` -> S->all `{"t":"own","g":gid,"c":newController}`.
- Population policy `host`: only the host keeps normal ambient traffic; every other client sets ped/vehicle density multipliers to 0 each frame and mirrors the host's entities.

Suggested `fl/a/b` meaning: ped `fl` = state bits as above, `a` = health, `b` = weapon hash; vehicle `fl` bits 1 engine on, 2 lights, 4 siren, 8 horn, `a` = colours (primary | secondary << 8), `b` = driver gid or 0.
