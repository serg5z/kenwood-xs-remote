# syntax=docker/dockerfile:1
# The bootstrap tools run natively on Apple Silicon. Target packages come
# exclusively from Raspbian and Raspberry Pi repositories, not Debian armhf.
FROM --platform=$BUILDPLATFORM debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f AS bootstrap
RUN apt-get update && apt-get install -y --no-install-recommends \
    debootstrap ca-certificates curl gnupg gpgv && rm -rf /var/lib/apt/lists/*
# debootstrap otherwise uses wget's long default read timeout.
RUN printf '%s\n' '#!/bin/sh' 'exec /usr/bin/wget --timeout=30 --tries=3 "$@"' \
    > /usr/local/bin/wget && chmod +x /usr/local/bin/wget
RUN --mount=type=cache,target=/downloads \
    curl -fsSL --connect-timeout 15 --max-time 90 https://archive.raspbian.org/raspbian.public.key -o /raspbian.asc \
    && gpg --dearmor -o /raspbian.gpg /raspbian.asc \
    && debootstrap --foreign --arch=armhf --variant=minbase --include=ca-certificates \
       --cache-dir=/downloads \
       --keyring=/raspbian.gpg trixie /rootfs https://archive.raspbian.org/raspbian \
    && cp /raspbian.gpg /rootfs/usr/share/keyrings/raspbian.gpg \
    && curl -fsSL --connect-timeout 15 --max-time 90 https://archive.raspberrypi.org/debian/raspberrypi.gpg.key \
       -o /raspberrypi.asc \
    && gpg --dearmor -o /rootfs/usr/share/keyrings/raspberrypi.gpg /raspberrypi.asc
# Legacy public-key URLs contain SHA1 self-certifications rejected by Trixie's
# sqv policy since February 2026. Use refreshed publisher keyrings, not a policy
# override: Raspbian's signed package and Raspberry Pi's pinned upstream fix.
RUN cp /rootfs/usr/share/keyrings/raspbian-archive-keyring.gpg \
       /rootfs/usr/share/keyrings/raspbian.gpg \
    && curl -fsSL --connect-timeout 15 --max-time 90 \
       https://raw.githubusercontent.com/raspberrypi/rpi-image-gen/1f5851b24a6ac25b91edfaed5f2b59e770d0ac9e/keydir/raspberrypi-archive-keyring.gpg \
       -o /rootfs/usr/share/keyrings/raspberrypi.gpg \
    && gpg --show-keys --with-colons /rootfs/usr/share/keyrings/raspbian.gpg \
       | grep -q A0DA38D0D76E8B5D638872819165938D90FDDD2E \
    && gpg --show-keys --with-colons /rootfs/usr/share/keyrings/raspberrypi.gpg \
       | grep -q CF8A1AF502A2AA2D763BAE7E82B129927FA3303E

FROM scratch
COPY --from=bootstrap /rootfs /
RUN /debootstrap/debootstrap --second-stage
RUN printf '%s\n' \
    'deb [signed-by=/usr/share/keyrings/raspbian.gpg] https://archive.raspbian.org/raspbian trixie main' \
    'deb [signed-by=/usr/share/keyrings/raspberrypi.gpg] https://archive.raspberrypi.org/debian trixie main' \
    > /etc/apt/sources.list
ARG GPIOD_VERSION=2.2.1-2+rpi1+deb13u1
RUN apt-get -o APT::Update::Error-Mode=any update && apt-get install -y --no-install-recommends \
    build-essential binutils file pkg-config ca-certificates \
    libcec-dev libgpiod-dev=${GPIOD_VERSION} libgpiod3=${GPIOD_VERSION} \
    && test "$(dpkg --print-architecture)" = armhf \
    && test "$(getconf GNU_LIBC_VERSION)" = 'glibc 2.41' \
    && rm -rf /var/lib/apt/lists/*
COPY verify.sh /usr/local/bin/verify-xs8-build
WORKDIR /src
