# syntax=docker/dockerfile:1
#
# zftpd — minimal Linux image with the web interface (zhttp) enabled.
#
#   docker build -t zftpd .
#   docker run --rm --network host -e ZFTPD_ADMIN_PASSWORD=change-me \
#       -v /srv/files:/srv/files -v zftpd-state:/var/lib/zftpd zftpd
#
# FTP passive data connections use random ports, so the container is meant to
# run with host networking (see docker-compose.yaml).  The web interface alone
# works fine behind a published port.

# ── Build stage ──────────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS build

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential binutils pkg-config python3 \
      libcurl4-openssl-dev libarchive-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Release builds use -Werror; newer GCCs raise a few warnings in third-party
# and legacy code that are harmless here, so those are demoted back.
RUN make TARGET=linux ENABLE_ZHTTPD=1 -j"$(nproc)" \
      CC="gcc -Wno-error=strict-overflow -Wno-error=unused-function -Wno-error=format-truncation -Wno-error=unused-result" \
 && install -m 0755 build/linux/release-zhttp/zftpd-linux-*-zhttp-v*.elf /zftpd

# ── Runtime stage ────────────────────────────────────────────────────────────
FROM debian:bookworm-slim

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libcurl4 libarchive13 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd -r zftpd && useradd -r -g zftpd -d /var/lib/zftpd -s /usr/sbin/nologin zftpd \
 && mkdir -p /srv/files /var/lib/zftpd \
 && chown zftpd:zftpd /srv/files /var/lib/zftpd \
 && chmod 0777 /srv/files /var/lib/zftpd

COPY --from=build /zftpd /usr/local/bin/zftpd

# Files served over FTP and the web interface, and the persisted web state
# (accounts, folder rules, share links).  Both are world-writable so the
# container can run as any uid (docker run --user / compose `user:`) and
# still write to a bind mount owned by that uid; the state files themselves
# are created with mode 0600.
VOLUME ["/srv/files", "/var/lib/zftpd"]

# Listening ports: ZFTPD_FTP_PORT / ZFTPD_HTTP_PORT (see docker-compose.yaml).
ENV ZFTPD_FTP_PORT=2121 \
    ZFTPD_HTTP_PORT=8888 \
    ZFTPD_ROOT=/srv/files \
    ZFTPD_STATE_DIR=/var/lib/zftpd
EXPOSE 2121 8888

USER zftpd
WORKDIR /srv/files
STOPSIGNAL SIGINT

CMD ["sh", "-c", "exec zftpd -p \"$ZFTPD_FTP_PORT\" -w \"$ZFTPD_HTTP_PORT\" -d \"$ZFTPD_ROOT\""]
