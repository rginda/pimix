#!/usr/bin/env bash
set -euo pipefail

pkill -f "ui/backend/server.py" || true
sleep 0.3

python3 -c '
import subprocess
subprocess.Popen(
    ["python3", "-u", "/home/rginda/src/pimix/ui/backend/server.py"],
    stdin=subprocess.DEVNULL,
    stdout=open("/tmp/pimix-ui.log", "w"),
    stderr=subprocess.STDOUT,
    start_new_session=True,
    close_fds=True,
)
'
sleep 1.2

if pgrep -f "ui/backend/server.py" > /dev/null; then
    IP_ADDR=$(hostname -I | awk '{print $1}')
    echo "PiMix UI Server is running!"
    echo "  URL (mDNS): http://rpi5-dev.local:8080"
    echo "  URL (IP):   http://${IP_ADDR}:8080"
    cat /tmp/pimix-ui.log
else
    echo "ERROR: PiMix UI Server failed to start:"
    cat /tmp/pimix-ui.log
    exit 1
fi
