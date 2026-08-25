# CombatForge — itch.io alpha deployment runbook

*Written 2026-07-15; Shipping/LAN-only release path updated 2026-08-25.*

## Facts that shaped this plan

- **Current packaged downloads are about 3.7 GB.** The itch.io
  **1 GB limit applies to the WEB uploader only** — `butler` pushes have no such cap and
  diff-upload future builds (only deltas go over the wire). So: butler, always.
- **Public uploads are Shipping builds.** Development remains available for local diagnostics, but
  the upload scripts refuse it unless the operator explicitly overrides the gate.
- **Join-by-IP already exists in the menu** (JOIN box + persisted last-IP), so VPN play
  needs zero code: LAN broadcast discovery dies over VPNs, direct IP connect works.
- **No official servers are available in this Alpha.** Solo, bots, LAN, and VPN work without the
  backend. The fleet implementation is dormant and no server/client coordinated deployment is needed.

## One-time setup (Tom — ~10 minutes, cannot be automated)

1. **itch.io account** (if none): https://itch.io/register
2. **Create the project page**: Dashboard → *Upload new project*
   - Title `CombatForge`, slug `combatforge`, Kind: **Downloadable**, Classification: Game
   - Release status: **In development** (this is the alpha marker)
   - Pricing: *No payments* (or donations — pairs with the in-game Buy Me a Coffee button)
   - Save as **Draft** for now (note: Draft pages do NOT allow downloads — visibility
     changes to Restricted in step 5 below)
3. **Install butler**:
   - Download: https://broth.itch.zone/butler/windows-amd64/LATEST/archive/default
   - Unzip to `C:\butler`, add `C:\butler` to PATH
   - Verify: `butler -V`
4. **Login** (browser OAuth): `butler login`

## Cook, verify, and push (repeatable)

```powershell
# Windows — UE 5.6, editor closed
.\Deploy\playtest\package-playtest.ps1 -Config Shipping
.\Scripts\Push-Itch.ps1        # dry run: manifest/hash/privacy/channel checks
# Launch Packaged\Release\Windows\CombatForge.exe and complete the Shipping smoke first.
.\Scripts\Push-Itch.ps1 -Push
```

```bash
# macOS — run on the MacBook with UE 5.6 + Xcode
git lfs install
git fetch origin release/epic-hardening-2026-08-24
git switch release/epic-hardening-2026-08-24
git pull --ff-only
git lfs pull
./Scripts/check-mac-env.command
./Scripts/Package-Mac.command Shipping
./Scripts/Push-Itch.command       # dry run
# Right-click the packaged CombatForge app → Open; verify Metal rendering and LAN joining first.
./Scripts/Push-Itch.command --push
```

Both upload scripts derive `0.1.0-alpha.N` from `PFBuild::NetProtocol`; never hand-type it. They
require `CombatForge-build.json`, a matching Shipping configuration/protocol/executable SHA-256,
no runtime `Saved` data, and no debug symbols. They push directories (not zip files) so butler can
diff/compress updates. The Windows default channel is `windows-alpha`; macOS is `mac-alpha`.

## Friends-only visibility (step 5)

On the project Edit page: **Visibility → Restricted** + set a **page password**.
Share URL + password (no itch account needed to download). Per-person alternative:
Distribute → **Download keys** (one revocable URL per friend).
Do NOT use Public-unlisted (anyone with the link) and do NOT share the Draft link
(downloads blocked in Draft).

## Paste into the itch page description (friend install runbook)

```
CURRENT ALPHA AVAILABILITY
No official servers are available. Play solo with bots, or host/join over LAN or a VPN such as
Tailscale. Quick Play and public official servers are upcoming features.

HOW TO INSTALL (Windows)
1. Download + unzip anywhere.
2. Run the CombatForge.exe in the TOP folder (it installs the VC++ prereq on first run).
3. Windows SmartScreen will warn (unsigned alpha): More info -> Run anyway.

HOW TO PLAY TOGETHER (VPN)
1. Join Tom's Tailscale network (he'll invite you).
2. HOST (Tom): click HOST LAN GAME. ALLOW the Windows Firewall prompt (check BOTH boxes).
   Find your Tailscale IP: tailscale ip -4  (it's the 100.x.y.z one).
   NOTE: the in-game "friends join" label shows your LAN IP — send friends the 100.x IP instead.
3. FRIENDS: type that 100.x.y.z IP into the JOIN box on the main menu and click JOIN.

LAN players must use the same Alpha version. A version mismatch blocks that connection to protect
the match from incompatible replicated game code, but it never blocks solo/bot play.
```

## Post-alpha size trim (deferred on purpose — needs recook + full in-game verification)

Potential cuts measured 2026-07-15 (uncompressed cooked sizes):
- `/Game/Survival_Character` force-cook → narrow to `Meshes` + used demo anims (~1.4 GB → ~50 MB)
- `/Game/QuantumCharacter` force-cook → narrow to `Mesh` + `Demo/Animations` + `Materials` (~730 MB → ~80 MB)
- `Industrial_Warehouse` in MapsToCook only feeds the default-OFF `pf.StreamWarehouseMap`
  cvar (~1.8 GB cooked) — keep for now so the option survives; drop when decided.
- SM5 shader archive (dual SM5+SM6 target) ~75 MB if all testers are on SM6-capable GPUs.
Verify after any trim: class menu cosmetics, all weapons, arena materials, both maps.

## Optional next-package code tweak

Make the hosting label VPN-aware: extend `GetLocalLanIp` (PFLoadingMenuWidget.cpp) to also
surface a 100.64.0.0/10 (Tailscale) / 25.x (Radmin) adapter address and render
"LAN <ip> / VPN <ip>". Pure UI, zero protocol change.
