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

## v4 additions
- state `s` gains `cp` (camera pitch) and `cy` (camera yaw), relayed in `w` rows: puppets aim where the player really aims.
- `{"t":"wl","l":0..5}` C->S shared wanted level (last change wins). S->C `{"t":"wl","l":N,"i":id}`; late joiners get the current level.
- `chat` is shown on the client as a feed ticker notification.
- NPC cars/peds that coincide with a player's car puppet are dropped on the receiver (no duplicates); the host removes its own car from the NPC stream the moment it sits in it.

## v5: full state
- entity row (owner -> server) gets an optional 14th element `d`; relayed rows are `[...,fl,a,b,ctl,d]`.
  - vehicle `d` = `[qx,qy,qz,qw, steer(reserved), bits, bodyHealth, engineHealth]`; bits: 1 engine, 2 lights, 4 high beam, 8 siren, 16 horn, doors open `<<8` (8 bits), tyres burst `<<16` (6 bits).
  - ped `d` = 12 x `[drawable, texture]` (clothes), sent once every 3 s.
  - ped `fl` adds 16 ragdoll, 2 shoot, 2048 dead (dead bodies are streamed too); on foot `b` = weapon hash (signed int32).
- state `s` gets `vd` (same layout as vehicle `d`) while in a car, relayed in `w` rows: the player's car puppet gets the same rotation, lights, doors, siren, damage.

## v6: events
- puppets (cars, peds) are proof against local bullets/fire/explosions/collisions; only the owner's state counts. Shots by a puppet are real bullets (aim ray 60 m from `cp`/`cy`), so the victim's own game applies the damage and reports it in `hp`/`fl`.
- vehicle `d[4]` = 1 exploded, 2 burning: receivers explode / ignite their copy.
- the server announces `<name> died` in chat when a player's dead flag (2048) rises.

## v10
- `wl` C->S: my own wanted level changed. S->C `{"t":"wl","l":N,"why":"..."}`: set my wanted level to at least N (crime or standing near a wanted player). No longer shared with everybody.
- state `s` gets `rv` (player id) or `rg` (npc car gid) when I am a passenger: receivers seat my puppet in THAT car instead of creating one. Relayed in `w` rows.
- vehicle detail `d`/`vd` may have 10 numbers: ..., primary colour, secondary colour.
- police peds/vehicles are streamed by a wanted player even if another player owns the ambient population.
