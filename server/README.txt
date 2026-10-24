GTAV MP server (needs only Node.js 18+, no other packages)

Windows: install Node.js from nodejs.org, then double-click start.bat
Termux:  pkg install nodejs, then: sh start.sh
Linux:   sh start.sh

Ports: UDP 30120 (game) and TCP 30121 (mods). Open both on the router/firewall.
Phone setting: server=<this machine's IP> (menu: Multiplayer tab).
Mods: put .zip files in mods/ (listed in mods/mods.json). Replacement and script mods are required for everyone.
Settings: server.json (port, httpPort, maxPlayers, motd).

Test without a friend: second terminal, in this folder:  node follow_bot.js   (a bot walks in a circle around you)

Hotspot / multiple network adapters: if a friend's join arrives but he never gets a reply (server prints "keeps re-sending join"),
set "host" in server.json to the IP of the interface he connects through (e.g. the hotspot IP like 192.168.x.1, see `ifconfig` in Termux),
and use that same IP as server= on every phone, including the host's own.

Automatic addresses: the server detects this device's IPs itself (hosts "auto" in server.json) and rescans every 5 s.
It prints "other phones use server=<ip>" for each one: friends on your hotspot use the hotspot IP, you use 127.0.0.1.

v2: shared world. Default start time is 22:00 (night) for everybody; change it in server.json ("world") or live:
  type in the server console, or as the host in chat:  /time 23 30   /weather RAIN   /freeze   /speed 30
Everyone on the server sees the same time and weather. NPC traffic and cars are mirrored from the host (PROTOCOL.md).

TEST ALONE (v9):  node test_bot.js      (second terminal/Termux session, next to the server)
Then type in the game chat (mp_say.txt):  !bot help
  !bot walk | stand | car [model]     bot walks a circle / drives a car around you
  !bot lights | beam | siren | horn | door | tire | explode | fire     (while !bot car) -> you must SEE it on his car
  !bot gun | rpg | shoot | shootme | die | revive      weapon, aiming/shooting (shootme = at you), death
  !bot npcs [off]      bot owns 4 test peds (walker, cop shooting, dead body, ragdoll) and 4 cars (lights+beam, 2 doors open + 2 tyres burst, siren, burning wreck)
  !bot wanted 3        sets the shared wanted level (your stars must change)
  !bot report          the bot tells you (feed) what it receives FROM you: your fl bits, weapon, camera, car lights/doors... = checks the SENDING side
Typing commands: simplest = in the terminal where test_bot.js runs, just type  car  /  lights  /  npcs  /  report ...  (no "!bot" needed).
In the game chat there is no keyboard: write into the file  /storage/emulated/0/Games/GTAV/mp_say.txt  (or in Termux: sh say.sh "!bot car"); the mod reads and deletes it within a second.

ONE TERMINAL (v9): in the SERVER console type  bot start  (join with the game first), then  bot car / bot lights / bot npcs / bot report / bot help ...  and  bot stop.

v10: wanted level is PER PLAYER. A player who shoots and kills a pedestrian/cop/player gets wanted (2, or 3 for a cop); anyone within 10 m of a wanted player becomes wanted too (server.json "wanted": {"radius": 10, "crimes": true}). Far players are not chased.
Passengers: you can get into somebody's car as passenger (his car or an NPC car); everybody sees you in HIS car, no second car is created. Test alone: bot command  ride  (bot sits in your car) or get into the bot's car (after  car).

v11: in the game chat any player can type  /tp <name or #id>  (or just /tp when there are two players) to teleport next to the other player.
Server console / host chat:  /players  now shows each player's position, who owns the area, wanted level and packet counters.
The client log (mp_log.txt) prints every 5 s where it thinks each other player is and how far away.
Tailscale: packets are kept under 1000 bytes (tunnel MTU 1280).
