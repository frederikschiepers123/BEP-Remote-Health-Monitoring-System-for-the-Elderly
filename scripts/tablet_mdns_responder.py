#!/usr/bin/env python3
"""Tablet-side mDNS responder — advertises a stable `tablet.local` name.

Runs in Termux on the Android tablet alongside Mosquitto. It lets the Pico
firmware resolve the broker by name (`tablet.local`) instead of a literal IP,
so the IP can change (new network, hotspot reconnect, DHCP lease) without
re-provisioning the device. This is the responder half of CLAUDE.md §8.2
step 3; the firmware half is `LWIP_DNS_SUPPORT_MDNS_QUERIES=1` in lwipopts.h.

Why python-zeroconf and not avahi: avahi in unrooted Termux needs dbus + system
integration and is unreliable. python-zeroconf is pure-Python, needs no root,
no dbus, and correctly implements RFC 6762 §6.7 legacy-unicast — it replies
*unicast* to a querier whose source port != 5353, which is exactly how lwIP's
dns.c mDNS client issues its query (from an ephemeral port). That unicast reply
lands on the Pico's own MAC, so no multicast-MAC-filter / IGMP gymnastics are
needed on either side.

What it advertises:
  - A record:      tablet.local           -> <current LAN IP>
  - service:       _mqtt._tcp.local :8883  (server=tablet.local)
    (the service record aligns with CLAUDE.md §15 step 13's `_mqtt._tcp.local`;
     the firmware only needs the A record, but advertising the service is free
     and lets `avahi-browse`/`dns-sd` discover the broker for debugging.)

It re-detects the LAN IP every few seconds and re-registers if it changed, so a
DHCP move is picked up automatically with no restart.

Usage:
    python3 tablet_mdns_responder.py            # advertises tablet.local
    python3 tablet_mdns_responder.py myname     # advertises myname.local

Stop with Ctrl-C (unregisters cleanly).
"""

from __future__ import annotations

import signal
import socket
import sys
import time

try:
    from zeroconf import ServiceInfo, Zeroconf
except ImportError:
    sys.exit("python-zeroconf not installed. In Termux: pip install zeroconf")


def primary_lan_ip() -> str:
    """Best-effort detection of the interface IP used for off-host traffic.

    Opens a UDP socket toward a public address (no packets sent) and reads back
    the local address the kernel would route through — this picks the active
    Wi-Fi IP rather than loopback, without depending on interface names that
    differ across Android builds.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def make_info(host_label: str, ip: str) -> ServiceInfo:
    server = f"{host_label}.local."
    return ServiceInfo(
        type_="_mqtt._tcp.local.",
        name=f"{host_label}._mqtt._tcp.local.",
        addresses=[socket.inet_aton(ip)],
        port=8883,
        server=server,
        properties={"role": "rmms-broker"},
    )


def main() -> int:
    host_label = sys.argv[1] if len(sys.argv) > 1 else "tablet"
    zc = Zeroconf()
    info: ServiceInfo | None = None
    cur_ip: str | None = None

    stop = {"flag": False}

    def handle_sig(signum, frame):  # noqa: ANN001
        stop["flag"] = True

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    print(f"[mdns] responder up — advertising {host_label}.local")
    try:
        while not stop["flag"]:
            ip = primary_lan_ip()
            if ip != cur_ip:
                if info is not None:
                    try:
                        zc.unregister_service(info)
                    except Exception as e:  # noqa: BLE001
                        print(f"[mdns] unregister warning: {e}")
                info = make_info(host_label, ip)
                try:
                    zc.register_service(info, allow_name_change=False)
                    cur_ip = ip
                    print(f"[mdns] advertising {host_label}.local -> {ip}:8883")
                except Exception as e:  # noqa: BLE001
                    print(f"[mdns] register failed for {ip}: {e}")
                    info = None
            # Poll for IP changes; cheap, and a DHCP move is picked up within 5 s.
            for _ in range(50):
                if stop["flag"]:
                    break
                time.sleep(0.1)
    finally:
        if info is not None:
            try:
                zc.unregister_service(info)
            except Exception:  # noqa: BLE001
                pass
        zc.close()
        print("\n[mdns] responder stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
