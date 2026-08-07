# Grok Roadmap — playtest 2026-08-06 fixes (simple scripting / balance)

Scope note (Tom's split): Claude owns the 3D/netcode bugs from this playtest — the
ammo/weapon desync, point-blank hit registration, wall "elevator", FP arms/camera, bot
double-gun, and server-perf work are **already handled or in progress in C++ — do not
touch those systems**. This file is the Grok work list: data/config/balance changes only.
Keep each change minimal and expect a human review pass.

Build check after edits: the game module must compile clean
(`Deploy\playtest\smoke-improvement.ps1` or a normal editor build). Do NOT package or
push to itch — Tom controls deploy timing.

---

## 1. LMG magazine sizes — floor at 180

Tom: "LMGs should have no less than 180 rounds of ammunition."

File: `Source/CombatForge/Combat/PFWeaponCatalog.cpp`, LMG category array `GLMGs`
(~lines 484–541). In each `MakeW(...)` call the magazine is the **second number in the
`ClassBurstCount, MagSize, FireRateBps` triple** (e.g. `3, 75, 10.0f`).

| Weapon | Line (approx) | Current mag | New mag |
|---|---|---|---|
| LMG 01 (`lmg_01`)      | ~492 `3, 75, 10.0f`  | 75  | 200 |
| LMG 02 (`lmg_02`)      | ~502 `3, 60, 6.0f`   | 60  | 180 |
| LMG 03 (`lmg_03`)      | ~512 `3, 100, 12.0f` | 100 | 200 |
| Minigun (`lmg_minigun`)| ~535 `3, 150, 18.0f` | 150 | 250 |

**Gotcha — MagSize is a `uint8` (max 255).** Do not exceed 255 anywhere. Also
`FPFWeaponDef::MagSize` feeds `UPFWeaponComponent::HopperCapacity` (also `uint8`) —
values above 255 wrap silently. The numbers above are safe.

Note: reserve ammo is a separate global (`MaxReserveAmmo = 120`,
`PFWeaponComponent.h:130`) — leave it unless Tom asks.

Do NOT "fix" the double-barrel (sg_03, mag 2) or hunt for an "RPK with 2 rounds" — those
playtest reports were a client/server weapon desync, already fixed in C++ (ClientCorrectKit,
CombatForgeCharacter.cpp). The catalog data was never wrong.

## 2. Shotgun 04 → full auto

File: `Source/CombatForge/Combat/PFWeaponCatalog.cpp`, `GShotguns[3]` ("Shotgun 04",
`sg_04`, ~lines 420–436).

- Change `Mode_SB, EPFFireMode::Single` → `Mode_SBA, EPFFireMode::Auto`.
- Leave `FireRateBps` (2.8) alone for the first pass — full-auto at 2.8 shells/sec with the
  existing 5° climb is the intended feel; tune only after Tom plays it.
- No other change needed: the server clamps allowed fire modes from this same catalog row
  (`ApplyWeaponLoadout` → `SetAllowedFireModes`), so data-only is sufficient.
- Update the big comment block above those lines (it currently explains why this is the
  only burst shotgun — rewrite to match the new full-auto reality, keep the "turn down
  pellets first if it dominates CQB" note).

## 3. Grenade carry count — triple

Tom: "Grenade shot count needs to triple."

File: `Source/CombatForge/Combat/PFWeaponComponent.h`
- Line ~91–92: `FragCount = 2` → `6`, `SmokeCount = 2` → `6` (initial values).
- Line ~135–136: `MaxFrag = 2` → `6`, `MaxSmoke = 2` → `6` (refill caps).

There are no per-class overrides anywhere — these four defaults are the only source of
truth (verified 2026-08-07). Respawn (`ServerResetLoadout`) and ammo-barrel refill
(`ServerRefillFromPickup`) both read `MaxFrag`/`MaxSmoke`, so nothing else to touch.

## 4. Server fleet cap 10 → 3

Tom: max 3 potential server instances (we never ran more than one and the box lagged).

**Two byte-identical copies — change BOTH or a redeploy resurrects the old cap:**
- `Deploy/pilot/Start-Fleet-OnBox.ps1`
- `Deploy/pilot/serve/Start-Fleet-OnBox.ps1`

Lines 32–35: the clamp `if ($Instances -gt 10) { ... $Instances = 10 }` → change both
`10`s (and the message) to `3`. Also update the "up to 10 active matches" comment on
line 1 and the "For the full 10..." sizing note on line 13. The default `-Instances`
parameter (line 21) is already 3 — leave it.

There is no instance cap anywhere in C++ or the backend worker — the script is the only
lever (verified 2026-08-07).

## 5. Ammo barrel: 10-second per-player cooldown + notification

Bug: a player could stand at a barrel and spam F to throw unlimited grenades.

Design (Tom's spec): per-player cooldown, 10 s, NOT per-barrel-global — three different
players in a row wait nothing; the same player waits 10 s. No persistent timer UI; just a
small private notice (lower-right toast is fine).

Implementation notes (from a code survey 2026-08-07 — follow these, they dodge real traps):

- **State goes on the player, not the barrel.** `SpawnAmmoBarrels`
  (`CombatForgeGameMode.cpp:2675`) spawns several barrels; per-barrel state lets players
  barrel-hop past the cooldown. Add `double LastBarrelRefillTime = -1000.0;` to
  `ACombatForgePlayerState` (or on `UPFWeaponComponent`, server-only, not replicated).
- **Gate in `APFAmmoBarrel::AuthorityInteract`** (`PFAmmoBarrel.cpp:252–299`) between the
  eliminated check (~:288) and `ServerRefillFromPickup()` (~:290). Both the host path and
  the remote-RPC path funnel through this one function, on authority.
- **Write the timestamp only when `ServerRefillFromPickup()` returns true** (it returns
  false when everything is already full) — a full-mag press must not burn the cooldown.
- **Exempt bots**: `PFBotController.cpp:272` calls `ServerRefillFromPickup()` directly,
  not through `AuthorityInteract`, so bots are unaffected — don't add anything there.
  Warm-up unlimited refills (`CombatForgeGameMode.cpp:2073`) likewise bypass — leave them.
- **Notification**: add a `UFUNCTION(Client, Reliable)` on `ACombatForgeCharacter`, e.g.
  `ClientBarrelCooldown(float RemainingSec)`, called from `AuthorityInteract` on a
  rejected press. Client side: forward to the HUD widget for a ~2 s fading text
  ("Resupply in Ns") lower-right. Precedent for an owner-client RPC on the character:
  `ClientCorrectKit` (CombatForgeCharacter.h). Do NOT put a countdown into
  `GetInteractPromptText()` — that function is polled client-side every frame and cannot
  see server-only state.

## 6. XP progression — double the rate, fast first 15 levels

Tom: "double the rate at which players earn XP… especially the first 15 levels. At least
one level per round. Overall, double the entire rate."

**Different repo:** `D:\projects\combatforge-api` (the Cloudflare Worker), file `src/xp.ts`.

The whole curve lives in one function — `costForLevel` (~line 52):

```ts
export function costForLevel(level: number): number {
  return Math.round((600 * Math.pow(1.06, level)) / 50) * 50;
}
```

Change to:

```ts
export function costForLevel(level: number): number {
  // Playtest 2026-08-06: progression felt dead. First 15 levels cost a flat 250 — one
  // completed match (completion bonus alone is 250 XP) = at least one level. Above 15,
  // half the original curve (base 600 → 300) = double the overall rate.
  if (level <= 15) return 250;
  return Math.round((300 * Math.pow(1.06, level)) / 50) * 50;
}
```

- `levelForXp` and `levelProgress` both derive from `costForLevel` — no other change.
- Levels recompute from stored `xpTotal` on the next report, so existing accounts jump
  forward automatically (intended).
- Do NOT touch `computeXpEvents` amounts or `CLAMP_MAX_PER_MATCH` — the ask is about the
  curve, not per-match mint sizes.
- Verify with `npx tsc --noEmit`. Do NOT deploy — Claude handles all API deploys (Tom
  2026-08-07); when your change is committed, Claude redeploys the worker.
- Context: the stale worker deploy (3 commits behind, missing `/v1/casual-report`) was
  found to be the "XP not happening" bug — Claude deployed the backlog 2026-08-07, so the
  live worker is now current. Your curve change is the only piece still owed.

## 7. Shotgun close-range damage — NO ACTION YET

The "closer than 3 feet does nothing" report was a hit-registration bug (balls spawned at
the muzzle, past/inside a hugging target). Claude fixed it in
`PFPaintballProjectile.cpp` (point-blank rescue sweep). With all 6–8 pellets now landing,
a point-blank volley is already a one-shot against the 5-credit chest-out threshold.
**Do not buff shotgun damage numbers until the next playtest confirms it's still weak.**
