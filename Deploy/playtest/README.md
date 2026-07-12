# PaintForge LAN / VPN playtest — **server piece**

Host a match on this PC. Friends join with `open <ip>:7777`.

You do **not** need a full shipping client build to host. The scripts fall back to the
**Unreal Editor** as a listen/dedicated-style host using project Content as-is.

---

## Mental model

```
Host PC                              Friend PC
────────                             ─────────
run-listen.ps1  (you play)     or
run-server.ps1  (headless)   ◄──── open 192.168.x.x:7777
```

No EOS/Steam yet — raw IP net driver only.

| Mode | Script | Host plays? | Needs packaged app? |
|------|--------|-------------|---------------------|
| **Listen** | `run-listen.ps1` / `start-host.bat` | Yes | No (editor works) |
| **Headless server** | `run-server.ps1` / `start-server.bat` | No | No (editor works) |

Default map `/Game/Maps/L_Graybox` · port **7777**.

---

## Host right now (recommended)

```powershell
cd D:\projects\paintforge
git checkout feat/playtest-server

# Option A — you play on this PC, friends join
.\Deploy\playtest\run-listen.ps1

# Option B — headless server; everyone (including you) joins as client
.\Deploy\playtest\run-server.ps1
# then on this PC: launch a normal game/editor client and:  open 127.0.0.1:7777
```

Share an address:

```powershell
.\Deploy\playtest\print-host-ips.ps1
```

Friends (once they have *any* runnable client — PIE on another machine is awkward; prefer a
packaged client when you have one):

```
open <your-lan-or-vpn-ip>:7777
```

---

## What the scripts try (in order)

**`run-server.ps1` (headless)**  
1. `PaintForgeServer.exe` if you ever have a source-engine server build  
2. Packaged / staged / Dev `PaintForge.exe -server -nullrhi`  
3. **`UnrealEditor.exe … -server -nullrhi`** ← works without packaging  

**`run-listen.ps1` (host plays)**  
1. Packaged / staged / Dev `PaintForge.exe Map?Listen`  
2. **`UnrealEditor.exe … -game Map?Listen`** ← works without packaging  

Force editor:

```powershell
.\Deploy\playtest\run-server.ps1 -PreferEditor
.\Deploy\playtest\run-listen.ps1 -PreferEditor
```

---

## Engine limitation (Launcher UE 5.6)

```
Server targets are not currently supported from this engine distribution.
```

So a true `PaintForgeServer` binary is **not** available until you use a **source-built** engine.
`Source/PaintForgeServer.Target.cs` is kept for that day. Playtest hosting does not depend on it.

---

## Docker

Docker Desktop Linux **cannot** run Win64 `PaintForge.exe`.  
For LAN/VPN on this machine: **use the PowerShell scripts**, not Docker.

`docker/` is a stub for a future Linux VPS + Linux cook only.

---

## Claude-safe

This folder + `PaintForgeServer.Target.cs` only. No GameMode / UI / combat edits.

---

## Later (when the app “looks and plays better”)

1. `package-playtest.ps1` — cook a Windows client zip for friends  
2. Copy zip + `connect.ps1` to their machines  
3. Host with `run-server.ps1` or `run-listen.ps1` on the staged/packaged exe  

Until then, editor-hosted listen/server is enough to validate join + phase loop on the network.

---

## Troubleshooting

| Issue | Fix |
|-------|-----|
| Timeout | Wrong IP (use VPN IP over Tailscale/etc.); firewall 7777 |
| Editor host black screen then exit | Map path wrong; confirm `Content/Maps/L_Graybox.umap` exists |
| Game.exe fails, no window | Dev binary without cook — use `-PreferEditor` |
| Friends on VPN can’t join | Share VPN IP from `print-host-ips.ps1`; allow inbound on VPN |
