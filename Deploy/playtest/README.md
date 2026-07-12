# PaintForge LAN / VPN playtest server

Host a match on this PC; friends on your LAN or VPN join by **IP:port**.

## How it works

```
┌──────────────────────────┐     UDP/TCP 7777      ┌─────────────────────┐
│  Host PC (you)           │ ◄───────────────────► │ Friend PC           │
│  Server process          │                       │ Packaged client     │
│  (listen OR headless)    │                       │ open 192.168.x.x:7777│
└──────────────────────────┘                       └─────────────────────┘
```

PaintForge has **no EOS/Steam matchmaking yet** (architecture D12). Join = engine connect string:

```
open 192.168.1.42:7777
```

Default map: `/Game/Maps/L_Graybox` · Default port: **7777**

### Two host modes

| Mode | Command style | You play on host PC? | When to use |
|------|---------------|----------------------|-------------|
| **Listen server** | `PaintForge.exe Map?Listen` | **Yes** (host is also a player) | Easiest playtest |
| **Headless `-server`** | `PaintForge.exe Map -server -nullrhi` | No — join with a second client | Host PC is “server only” |

### Engine note (important)

Epic **Launcher** UE 5.6 builds **cannot compile `TargetType.Server`**  
(`Server targets are not currently supported from this engine distribution`).

- `Source/PaintForgeServer.Target.cs` is in the repo for a future **source-built** engine.
- **Today’s scripts use the normal game binary** (`PaintForge.exe`) in listen or `-server` mode — that works with the Launcher engine and does not need a special server target.

---

## Claude-safe scope

This branch only adds deploy scripts, docs, a future Server target file, and gitignore entries.
It does **not** change GameMode, GameState, UI, combat, or lobby code.

---

## Quick start (recommended)

### A. You host and play (listen server)

1. Package or use an existing staged client (see below).
2. On the host PC:

```powershell
cd D:\projects\paintforge
.\Deploy\playtest\run-listen.ps1
```

3. Share an IP:

```powershell
.\Deploy\playtest\print-host-ips.ps1
```

4. Friends launch their client and:

```
open <your-ip>:7777
```

or:

```powershell
.\connect.ps1 -Server <your-ip>
```

### B. Headless-ish server (you join as a client)

```powershell
.\Deploy\playtest\run-server.ps1
# then on this PC or another:
.\Deploy\playtest\connect.ps1 -Server 127.0.0.1   # or LAN/VPN IP
```

---

## Getting a client build for friends

### Option 1 — already staged

If you previously packaged from the editor, you may already have:

`Saved\StagedBuilds\Windows\PaintForge.exe` (or under a subfolder)

Zip that whole `Windows` (or `PaintForge`) tree and copy it to friends (network share / USB).

### Option 2 — package via script

Editor **closed**:

```powershell
.\Deploy\playtest\package-playtest.ps1
```

Output under `Packaged\Playtest\` (gitignored). Zip the **Windows client** folder for friends.

First cook can take a long time.

### Option 3 — editor PIE (dev only)

PIE multiplayer is for local dev. For other PCs on the VPN, use a packaged client.

---

## Docker Desktop

Your Docker Desktop is in **Linux** mode.

| Approach | Works? |
|----------|--------|
| Run `run-server.ps1` on **Windows host** | **Yes — use this for playtest** |
| Put **Win64** `PaintForge.exe` in a Linux container | **No** |
| Linux dedicated server in Docker | Only after a **Linux** cook + source/Linux toolchain (advanced) |

`Deploy/playtest/docker/` is a **stub** for a future Linux VPS. It is not required for LAN/VPN play on this PC.

---

## VPN checklist (Tailscale / ZeroTier / etc.)

1. Host + friends on the same VPN mesh.
2. Host runs `run-listen.ps1` or `run-server.ps1`.
3. `print-host-ips.ps1` → give friends the **VPN** IP (not only LAN).
4. Friends: `open <vpn-ip>:7777`.
5. If join fails: Windows Firewall on host (script can open 7777), VPN “allow inbound”, confirm host log shows join attempts.

---

## Scripts

| Script | Purpose |
|--------|---------|
| `run-listen.ps1` | Host + play (recommended) |
| `run-server.ps1` | Headless-style `-server -nullrhi` on game exe |
| `print-host-ips.ps1` | IPs to share |
| `connect.ps1` | Friend join helper (ship next to client exe) |
| `package-playtest.ps1` | Cook/stage client for distribution |
| `build-server.ps1` | Only works with **source** engine Server target |
| `docker/*` | Future Linux container layout |

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Timeout joining | Wrong IP (try VPN IP); firewall UDP/TCP 7777 |
| Server window closes immediately | Need a **cooked/packaged** game, not only editor DLL |
| Friends can’t see host | Host bound to listen; check `print-host-ips`; disable strict firewall temporarily |
| “Server targets not supported” | Expected on Launcher engine — use `run-listen.ps1` / `run-server.ps1` |
| Host cosmetics weird | Listen-server host is also authority — known asymmetry; or use headless server + all clients |

---

## What you install where

| Machine | Needs |
|---------|--------|
| **Host (this PC)** | Repo or packaged build + `run-listen.ps1` / `run-server.ps1` |
| **Friend** | Packaged **Windows client** zip only (no UE editor, no full repo) |
