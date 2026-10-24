#!/bin/sh
# Termux: send a chat line / bot command into the game.   sh say.sh "!bot car"
echo "$*" >> /storage/emulated/0/Games/GTAV/mp_say.txt
