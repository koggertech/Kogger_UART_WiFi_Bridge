#!/bin/sh
# Install the Wi-Fi bridge daemon on the HeadUnit. Run from this directory: sudo sh install.sh
set -eu
install -d /opt/espwifi
install -m 0644 wbframe.py /opt/espwifi/
install -m 0755 espwifi_bridge.py espwifi_ctl.py /opt/espwifi/
install -m 0644 99-espwifi.rules /etc/udev/rules.d/
install -m 0644 nm-espwifi.conf /etc/NetworkManager/conf.d/espwifi.conf
install -m 0644 espwifi.service /etc/systemd/system/
udevadm control --reload
udevadm trigger --subsystem-match=tty
systemctl reload NetworkManager || true
systemctl daemon-reload
systemctl enable --now espwifi.service
sleep 2
systemctl --no-pager --lines=5 status espwifi.service || true
