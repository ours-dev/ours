#!/bin/bash
# Entrypoint for the Ours dev container (runs as root).
#
# Two persistence duties, then drop privileges to the developer user:
#
# 1. Record every package installed on top of the image's baseline
#    (/etc/ours-dev-base-packages, snapshot at build time) into
#    /home/developer/.ours-dev-packages — a plain text list inside the
#    persistent home volume.
# 2. Reinstall anything from that list the (possibly rebuilt) image is
#    missing, so apt-installed tools survive image rebuilds.
#
# /usr/local and /opt are separate named volumes (see dev.compose), so
# tools installed there never even need reinstalling.

set -u

DEVELOPER_HOME=/home/developer
BASE_LIST=/etc/ours-dev-base-packages
PERSIST_LIST="$DEVELOPER_HOME/.ours-dev-packages"

installed_now() { dpkg-query -W -f='${Package}\n' 2>/dev/null | sort -u; }

# 1. Merge this image's extra packages into the persistent list.
if [ -f "$BASE_LIST" ]; then
    EXTRA=$(comm -13 <(sort -u "$BASE_LIST") <(installed_now))
    if [ -n "$EXTRA" ]; then
        touch "$PERSIST_LIST"
        printf '%s\n' "$EXTRA" | sort -u >> "$PERSIST_LIST"
        sort -u "$PERSIST_LIST" -o "$PERSIST_LIST"
    fi
fi

# 2. Install persisted packages the current image lacks.
if [ -f "$PERSIST_LIST" ]; then
    MISSING=$(comm -13 <(installed_now) <(sort -u "$PERSIST_LIST") || true)
    if [ -n "$MISSING" ]; then
        echo "ours-dev: restoring packages into the rebuilt image..."
        apt-get update -qq || { echo "ours-dev: apt update failed (offline?), skipping restore"; }
        # shellcheck disable=SC2086
        apt-get install -y -qq $MISSING || echo "ours-dev: some packages failed to restore"
    fi
fi

DEVELOPER_UID=$(id -u developer)
DEVELOPER_GID=$(id -g developer)

# Keep the mounted home volume owned by the developer user.
chown -R "$DEVELOPER_UID:$DEVELOPER_GID" "$DEVELOPER_HOME" 2>/dev/null || true

export HOME="$DEVELOPER_HOME"
exec setpriv --reuid="$DEVELOPER_UID" --regid="$DEVELOPER_GID" --init-groups "$@"
