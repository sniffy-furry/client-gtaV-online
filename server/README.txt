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
