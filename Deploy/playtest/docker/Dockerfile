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

# Fail the BUILD, not the container, when server-linux/ is still the empty placeholder. Without
# this, `docker compose up --build` reports a successful build and then the container exits 1 on
# every restart, which reads as a runtime fault rather than a missing cook. (P2-D7)
RUN if [ -z "$(find /server -type f -name CombatForgeServer -print -quit)" ]; then \
		echo "ERROR: no CombatForgeServer binary in Deploy/playtest/docker/server-linux/."; \
		echo "       Cook a LINUX dedicated server build and stage it there first — this image"; \
		echo "       cannot run Win64 binaries. For a Windows host playtest use ../run-server.ps1."; \
		exit 1; \
	fi

# UE often needs execute bit on the binary and any .sh wrapper
RUN chmod +x /server/CombatForgeServer 2>/dev/null || true; \
	chmod +x /server/*.sh 2>/dev/null || true; \
	find /server -type f -name "CombatForgeServer" -exec chmod +x {} \; 2>/dev/null || true

EXPOSE 7777/udp 7777/tcp

ENV PF_MAP=/Game/Maps/L_Graybox
ENV PF_PORT=7777
# Fleet key is runtime env (PF_SERVER_KEY), not bake-in.

# Default entry: adjust binary path if your cook layout differs
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
