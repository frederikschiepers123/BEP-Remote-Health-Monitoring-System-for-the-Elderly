#!/data/data/com.termux/files/usr/bin/bash

termux-wake-lock

echo "Mirror boot script ran at $(date)" >> ~/boot-test.log

cd ~/BEP-Remote-Health-Monitoring-System-for-the-Elderly/MagicMirror

node --run server >> ~/magicmirror.log 2>&1 &

sleep 15

am start -n de.ozerov.fully/.MainActivity