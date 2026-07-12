# PaintForge — playtest checklist

Use this for the first human / multiplayer session on **current `main`**.

## Before

- [ ] `git pull origin main`
- [ ] UE 5.6 installed; project opens without missing map errors
- [ ] `Content/Maps/L_Graybox` loads
- [ ] Editor closed if you will package

## Host (this PC)

```powershell
cd D:\projects\paintforge   # or your clone
.\Deploy\playtest\run-listen.ps1
# or headless: .\Deploy\playtest\run-server.ps1
.\Deploy\playtest\print-host-ips.ps1
```

Share LAN/VPN IP + port **7777**.

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
| Host works, client dead input | `Saved/Logs/PaintForge.log` on both |
| Join mid-round | Client should not get false breakout spam |
| No arenas for Improvement | Play a Creative match once, or use smoke seed |
| Port blocked | `print-host-ips` + Windows firewall (script may prompt UAC) |

## Success signals

- Full Lobby → (Build) → Combat → Vote → Results without crash  
- Second player sees team colors, scores, and can tag  
- Host has a new file under `Saved/Arenas/` after a match with geometry  

## Out of scope for this playtest

- Fab assets at project root (not wired)  
- EOS matchmaking  
- Shipping store packaging polish  
