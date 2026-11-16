# Native systemd test lab. On Apple Silicon build with --platform linux/arm64.
# This supports Linux service and fixture pacman transactions, not an Omarchy desktop.
FROM debian:trixie
ENV container=docker LANG=C.UTF-8
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    systemd systemd-sysv dbus dbus-user-session python3 sudo ca-certificates \
    qt6-base-dev qt6-declarative-dev qt6-svg-dev libarchive-dev build-essential git \
    pacman-package-manager makepkg fakeroot zstd libarchive-tools \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m -u 501 builder && systemctl mask \
    systemd-binfmt.service systemd-modules-load.service systemd-sysctl.service \
    systemd-udevd.service systemd-udev-trigger.service getty.target
COPY tools/agent-lifecycle/lab-init.sh /usr/local/sbin/oc-lab-init
STOPSIGNAL SIGRTMIN+3
ENTRYPOINT ["/bin/sh", "/usr/local/sbin/oc-lab-init"]
