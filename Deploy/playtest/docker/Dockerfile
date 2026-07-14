# Linux dedicated server image for CombatForge playtests (VPS / Docker Desktop Linux).
#
# PREREQ: cook a LINUX dedicated server build and place it at:
#   Deploy/playtest/docker/server-linux/
# containing CombatForgeServer (or the staged LinuxServer tree).
#
# Win64 server binaries will NOT run in this image.
# For Windows host playtest, use ../run-server.ps1 instead.

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
	ca-certificates \
	libx11-6 libxext6 libxrender1 libxrandr2 libxi6 libxss1 \
	libgl1 libglu1-mesa libasound2 \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /server
COPY server-linux/ /server/

# UE often needs execute bit on the binary and any .sh wrapper
RUN chmod +x /server/CombatForgeServer 2>/dev/null || true; \
	chmod +x /server/*.sh 2>/dev/null || true; \
	find /server -type f -name "CombatForgeServer" -exec chmod +x {} \; 2>/dev/null || true

EXPOSE 7777/udp 7777/tcp

ENV PF_MAP=/Game/Maps/L_Graybox
ENV PF_PORT=7777

# Default entry: adjust binary path if your cook layout differs
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
