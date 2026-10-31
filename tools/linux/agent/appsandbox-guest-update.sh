#!/bin/bash
# Refresh this VM's guest components from the source tree the host just sent
# (the agent extracted it to /var/lib/appsandbox/update and starts this script).
# Linux counterpart of the helper refresh Windows VMs get at start.
#
# Usage: appsandbox-guest-update.sh <stamp>
set -o pipefail
STAMP=$1
ROOT=/var/lib/appsandbox/update/tools/linux
STAMP_FILE=/etc/appsandbox-guest-stamp

fail() {
    echo "guest refresh failed: $1"
    exit 1
}

cd "$ROOT/agent" || fail "no source tree"
make -j"$(nproc)" 2>&1 | tail -20 || fail "make"
make install PREFIX=/usr/local 2>&1 | tail -5 || fail "make install"

# Units not covered by the Makefile's install list.
install -m 0644 "$ROOT"/agent/systemd/appsandbox-agent.service \
                "$ROOT"/agent/systemd/appsandbox-audio.service \
                "$ROOT"/agent/systemd/appsandbox-display.service \
                "$ROOT"/agent/systemd/appsandbox-input.service /etc/systemd/system/
if [ -f "$ROOT/agent/asound-appsandbox.conf" ]; then
    install -m 0644 "$ROOT/agent/asound-appsandbox.conf" /etc/asound.conf
fi
for f in "$ROOT"/agent/51-appsandbox-audio.conf; do
    [ -f "$f" ] || continue
    install -d /etc/wireplumber/wireplumber.conf.d
    install -m 0644 "$f" /etc/wireplumber/wireplumber.conf.d/
done
systemctl daemon-reload

# VMs built before first boot did this.
mkdir -p /etc/systemd/user
ln -sf /dev/null /etc/systemd/user/gnome-initial-setup-upgrade-login.service
ln -sf /dev/null /etc/systemd/user/gnome-initial-setup-first-login.service

# The display driver only changes at a reboot, and only if its source did.
ver=$(awk -F= '/^PACKAGE_VERSION=/{gsub(/"/,"",$2); print $2}' "$ROOT/asb_drm/dkms.conf")
[ -n "$ver" ] || ver=1.0.0
if ! diff -rq "$ROOT/asb_drm" "/usr/src/asb_drm-$ver" >/dev/null 2>&1; then
    dkms remove -m asb_drm -v "$ver" --all >/dev/null 2>&1
    rm -rf "/usr/src/asb_drm-$ver"
    cp -r "$ROOT/asb_drm" "/usr/src/asb_drm-$ver"
    dkms add -m asb_drm -v "$ver" 2>&1 | tail -2
    dkms install -m asb_drm -v "$ver" -k "$(uname -r)" --force 2>&1 | tail -3 \
        || fail "asb_drm build"
    echo "display driver updated; it loads at the next boot"
fi

mkdir -p /var/lib/appsandbox
echo "$STAMP" > "$STAMP_FILE"

systemctl restart appsandbox-input.service appsandbox-display.service appsandbox-audio.service
# Last, since it runs the connection that started all this.
systemctl restart appsandbox-agent.service
echo "guest refresh complete: $STAMP"
