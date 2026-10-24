TERMUX PATCH KIT
One time:  termux-setup-storage   (allow storage), then extract this 7z somewhere (e.g. ~/termux_patch_kit).
Every build: copy the new libmpclient.so into the Download folder, then
    sh ~/termux_patch_kit/patch.sh
-> /storage/emulated/0/Download/XHYNMP_mp5.apk, install it over the old app (same package + same key, no uninstall).
KEEP mpmod.key.pem: it is your signing key. Lose it = you must uninstall the app before installing a new build. Do not share it.
