# Crash investigation — kids playtest (2026-07-12)

## What we know from host logs

On the **host** (`D:\projects\paintforge\Saved\Logs`):

- No `Fatal error` / `Assertion failed` / access-violation stacks.
- Client disconnects show as normal net cleanup, e.g.  
  `UNetConnection::Close … RemoteAddr: 192.168.68.63 … Result=Cleanup`
- Process exits look like **clean engine shutdown** (`LogExit: Exiting`) more often than a hard fault.
- Repeated: missing soft dependency  
  `/Game/Weapons/Rifle/Materials/MI_Weapon_Rifle` when loading `SM_Rifle`  
  (Lyra material not in project — remapped in code to `M_PF_Rifle`).

**Conclusion so far:** many “crashes” on kids may be:

1. **Client hard-exit** (OOM, GPU TDR, force-close) while host stays up  
2. **ensure/assert freeze** on Development builds (looks frozen, then closed)  
3. **Disconnect** without a UE crash dialog  

Host-only logs cannot prove client crash cause. We need **kid PC evidence**.

## High-risk code vectors (hardened)

| Risk | Mitigation |
|------|------------|
| `check(PawnOwner)` in CMC prediction during teardown | Soft null return + log (no hard check) |
| Mannequin `ensureMsgf` on mesh origin | Downgraded to `UE_LOG` Warning |
| SM_Rifle missing Lyra material | Force all material slots → `M_PF_Rifle` |
| Ghost rejoin / dual roster | Destroy PS on Logout + scrub (PR #7) |
| Long net timeout ghosts | 12s connection timeout (PR #7) |

## Host breadcrumbs (new)

Every **30s** the host logs:

```
CRASH_BC phase=… humans=… bots=… ghosts=… scores=… [names]
```

After a kid disappears, open host `Saved/Logs/PaintForge.log` and search `CRASH_BC` for the last line before they vanished.

## Client → host log ship (preferred)

Remote kids automatically **stream their log lines to the host** over the game connection
(~1 s cadence). After a crash, the host still has almost everything:

```
Saved/ClientLogs/<PlayerName>_<timestamp>.log
```

On the host after a bad session:

```powershell
# open the latest files
explorer D:\projects\paintforge\Saved\ClientLogs
```

Or:

```powershell
.\Deploy\playtest\collect-crash-evidence.ps1
```

(now also copies `Saved/ClientLogs/`)

**Limits:** last ~1–2 s before a hard kill may be missing (buffer not flushed).  
Listen-host’s own log stays in `Saved/Logs/` (not re-shipped).

## Collect evidence (next session)

### On each kid PC that crashed (optional if ClientLogs exist on host)

1. Unzip the game folder they used.
2. Run (copy script next to the zip or from the project):

```powershell
# From the project machine, or copy this script onto a USB:
.\Deploy\playtest\collect-crash-evidence.ps1 -ProjectRoot "C:\Games\PaintForge"
```

If the script isn’t on the kid PC, manually zip:

- `…\Saved\Logs\` under the game install (if present)
- `%LOCALAPPDATA%\CrashReportClient\Saved\Logs\`
- `%LOCALAPPDATA%\CrashReportClient\Saved\Reports\` (if any)

### On the host after a bad match

```powershell
cd D:\projects\paintforge
.\Deploy\playtest\collect-crash-evidence.ps1
```

Send **one zip per machine** (host zip should include ClientLogs).

## Repro matrix (controlled)

Run with **2–3 clients**, Development package rebuilt after crash-hardening:

| # | Setup | Watch |
|---|--------|--------|
| A | Skirmish Play-Only 4v4 bots | baseline stability 10 min |
| B | Same, one kid **Alt+F4** mid-fight | host ghost scrub, rejoin once |
| C | Domination 5+ min | pad height + bot objective stress |
| D | CTF with bots | flag carry / drop / return |
| E | Weakest kid PC only + 6v6 bots | OOM / texture stream stress |

## Packaging notes for weak kids PCs

Rifle textures are large (10–20 MB each). If crashes correlate with first seeing the rifle:

- Rebuild package after material remaps (this branch).
- Later: cook max texture size 1024 for playtest builds.

## What “success” looks like

- No silent client exit for 20+ min of A/C  
- Host log has continuous `CRASH_BC` lines  
- If a client still dies: zip from `collect-crash-evidence.ps1` contains a CRC report or last log lines with `Fatal` / `Exception`
