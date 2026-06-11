#!/usr/bin/env bash
# Stop and remove the RMMS aggregator service. Leaves /var/lib/rmms (the data /
# audit trail) and /etc/rmms (config + certs) in place unless --purge is given.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }

systemctl disable --now rmms-aggregator.service 2>/dev/null || true
rm -f /etc/systemd/system/rmms-aggregator.service
systemctl daemon-reload
(cd /opt/rmms-aggregator 2>/dev/null && docker compose down) || true
rm -rf /opt/rmms-aggregator

if [ "${1:-}" = "--purge" ]; then
    echo "purging data + config"
    rm -rf /var/lib/rmms /etc/rmms
fi
echo "uninstalled"
