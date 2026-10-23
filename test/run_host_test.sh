#!/bin/sh
# Runs the end-to-end host test against the v2 relay server.
SERVER_DIR=${SERVER_DIR:-/tmp/v2/gtav-mp-server}
cd "$SERVER_DIR" && node server.js > /tmp/server_test.log 2>&1 &
SP=$!
sleep 1.2
timeout 90 /tmp/host_test
RC=$?
kill $SP 2>/dev/null
exit $RC
