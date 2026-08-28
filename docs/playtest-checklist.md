# CombatForge — playtest checklist

Use this for the LAN-only Alpha release candidate.

## Before

- [ ] Pull the release branch/commit identified in `CombatForge-build.json`
- [ ] UE 5.6 installed; project opens without missing map errors
- [ ] `Content/Maps/L_Graybox` loads
- [ ] Editor closed if you will package
- [ ] `python Scripts\ci_release_check.py` passes (same gate CI runs)
- [ ] `.\Scripts\run-tests.ps1` passes — module build + C++ automation tests. **CI cannot run
      this**: GitHub's runners have no Unreal install, so compiling and the `CombatForge.*` tests
      only ever happen on a machine with the engine. A green PR is not a green build.

## Host (this PC)

```powershell
cd D:\projects\combatforge   # or your clone
.\Deploy\playtest\run-listen.ps1
# or headless: .\Deploy\playtest\run-server.ps1
.\Deploy\playtest\print-host-ips.ps1
```

Share LAN/VPN IP + port **7777**.

## Alpha availability gate

- [ ] Standalone menu says **NO OFFICIAL SERVERS AVAILABLE** and **LAN / VPN play is available now**
- [ ] No LOG IN, QUICK PLAY, SERVERS, match-code, or START NEW MATCH official-service control is visible
- [ ] QUICK START and START GAME work with the network disconnected
- [ ] All five local class slots and all local weapon choices remain usable without an account
- [ ] HOST LAN GAME and direct-IP JOIN remain visible and functional

## Friends

- Packaged client preferred, or editor with same `main`
- Console: `open <host-ip>:7777`

## Recommended first match (kids)

1. Lobby → **TYPE** = Skirmish  
2. **MODE** = Play-Only (skip build)  
3. **FORMAT** = 4v4 (bots fill empty slots)  
4. Host **Enter** force start (or all ready)  
5. Shoot, die, respawn; first team to tag target or timer wins  
6. Vote thumbs → Results → back to lobby  

## Full loop (build)

1. MODE = Creative  
2. TYPE = Skirmish or Elimination  
3. Build fort → ready → combat → vote → results  
4. Confirm `Saved/Arenas/*.json` was written on host  

## Objectives (optional second match)

- CTF / Dom / HP — bots will seek flags/points  
- Tab scoreboard: mode unit + scores; FLAG / point markers  

## Automated gates (optional)

```powershell
.\Deploy\playtest\smoke-improvement.ps1   # should PASS
.\Deploy\playtest\smoke-package.ps1       # cook + boot PASS (slow first time)
```

## Watch for

| Issue | Where to look |
|-------|----------------|
| Host works, client dead input | `Saved/Logs/CombatForge.log` on both |
| Join mid-round | Client should not get false breakout spam |
| No arenas for Improvement | Play a Creative match once, or use smoke seed |
| Port blocked | `print-host-ips` + Windows firewall (script may prompt UAC) |

## Success signals

- Full Lobby → (Build) → Combat → Vote → Results without crash  
- Second player sees team colors, scores, and can tag  
- Host has a new file under `Saved/Arenas/` after a match with geometry  
- Solo/bot play remains available after a failed LAN connection attempt

## Out of scope for this playtest

- Fab assets at project root (not wired)  
- Official servers, public matchmaking, Quick Play, and account progression
