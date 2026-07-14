# CombatForge — LAN / VPN playtest

Host a match on this PC. Friends join with `open <ip>:7777`.

You do **not** need a full shipping client to host. Scripts fall back to the **Unreal Editor**
using project Content as-is. A Development package also works if you have one.

**Always use `main` (or the latest merge). No feature-branch checkout required.**

---

## Mental model

```
Host PC                              Friend PC
────────                             ─────────
run-listen.ps1  (you play)     or
run-server.ps1  (headless)   ◄──── open 192.168.x.x:7777
```

No EOS/Steam yet — raw IP net driver only.

| Mode | Script | Host plays? | Needs package? |
|------|--------|-------------|----------------|
| **Listen** | `run-listen.ps1` / `start-host.bat` | Yes | No (editor works) |
| **Headless server** | `run-server.ps1` / `start-server.bat` | No | No (editor works) |

Default map `/Game/Maps/L_Graybox` · port **7777**.

---

## Host right now (recommended)

```powershell
cd D:\projects\combatforge   # or combatforge-grok
git pull origin main

# Option A — you play on this PC, friends join
.\Deploy\playtest\run-listen.ps1

# Option B — headless server; everyone (including you) joins as client
.\Deploy\playtest\run-server.ps1
# then launch editor/game client and console:  open 127.0.0.1:7777
```

Share an address:

```powershell
.\Deploy\playtest\print-host-ips.ps1
```

Friends (any runnable client — packaged preferred for remote):

```
open <your-lan-or-vpn-ip>:7777
```

Force editor host (ignore packaged exe):

```powershell
.\Deploy\playtest\run-listen.ps1 -PreferEditor
.\Deploy\playtest\run-server.ps1 -PreferEditor
```

---

## Lobby cheatsheet (host)

Hold **Tab** for cursor / scoreboard in lobby.

| Control | Action |
|---------|--------|
| Click **MODE** | Creative / Improvement / Play-Only |
| Click **TYPE** | Elim / FFA / Skirmish / CTF / Dom / HP |
| Click **FORMAT** | 4v4 ↔ 6v6 (bots fill) |
| Click a player row | Cycle team (not FFA) |
| **F** | Ready |
| **Enter** | Force start (host) |

### Console (host, `~`)

| Command | Meaning |
|---------|---------|
| `PFMode 0/1/2` | Creative / Improvement / Play-Only |
| `PFType 0..5` | Elim / FFA / Skirmish / CTF / Dom / HP |
| `PFFormat 4` or `6` | Team size |
| `PFForceStart` | Force lobby countdown |

### First-session recommendations

| Goal | Setup |
|------|--------|
| **Fast fight (kids)** | TYPE=Skirmish, MODE=Play-Only, FORMAT=4v4, bots on |
| **Build + fight** | MODE=Creative, TYPE=Skirmish |
| **Objectives** | CTF / Dom / HP (bots will try to play them) |
| **Solo tags** | TYPE=FFA (forces Play-Only) |

Defaults on boot: **Skirmish**, **Creative**, **4v4**, bots fill.

---

## Client logs on the host (after kids crash)

Remote clients stream log lines to the host every ~1 s over the game connection.

On the **host** after a session:

```
Saved\ClientLogs\<PlayerName>_<timestamp>.log
```

```powershell
explorer .\Saved\ClientLogs
.\Deploy\playtest\collect-crash-evidence.ps1   # zips logs + ClientLogs
```

## Smoke tests (automated)

```powershell
# Improvement inject path (editor -game -nullrhi)
.\Deploy\playtest\smoke-improvement.ps1

# Full cook + packaged boot (Development)
.\Deploy\playtest\smoke-package.ps1

# Boot existing package only
.\Deploy\playtest\smoke-package.ps1 -SkipCook
```

---

## Package a client for friends

```powershell
.\Deploy\playtest\package-playtest.ps1
# Output: Packaged\Playtest\Windows\CombatForge.exe
```

Zip the `Windows` folder + `connect.ps1` for friends.

---

## Engine limitation (Launcher UE 5.6)

```
Server targets are not currently supported from this engine distribution.
```

True `CombatForgeServer` needs a source-built engine. Hosting does **not** require it —
use `run-listen` / `run-server` (game or editor + `-server -nullrhi`).

---

## Docker

Docker Desktop Linux **cannot** run Win64 `CombatForge.exe`.  
For LAN/VPN on this machine: **PowerShell scripts**, not Docker.

`docker/` is a stub for a future Linux VPS cook only.

---

## Asset dumps at project root

Fab/Lyra/Megascans folders parked next to the `.uproject` are **staging only** — not cooked.
Do not commit huge dumps. Import into `Content/` when wiring art (separate pass).

---

## Checklist

See [`docs/playtest-checklist.md`](../../docs/playtest-checklist.md) for a short operator list.
