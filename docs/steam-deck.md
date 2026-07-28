# Steam Deck support

Written 2026-07-28 on branch `feat/steam-deck` (stacked on `feat/handheld-windows`).
CombatForge runs on Steam Deck as the **Windows build under Proton** — there is no native
Linux build (deliberate: one binary, one NetProtocol, cross-plays with the live Windows
fleet; a Linux server/client would need the cross-toolchain and a second QA matrix).

⚠️ **Not run on a real Deck** (none owned). The compatibility story below is an audit of
what the game actually uses vs what Proton provides, plus the shared handheld work from
`feat/handheld-windows`. On-device verification is tracked in a GitHub issue (`handheld`
label).

---

## 1. What makes it work (already in the code)

Most Deck enablement landed in `feat/handheld-windows` and applies automatically:

- **Detection**: SteamOS sets `SteamDeck=1` for every process — it survives Proton into
  the Windows build and `FPFHandheldPlatform::Detect()` reads it. Windows-installed-on-Deck
  is covered too (CPU brand `AMD Custom APU 0405` LCD / `0932` OLED). Force with
  `-pfsteamdeck`, disable with `-pfnothandheld`.
- **First-run Deck preset** (one-shot, never re-stomps player changes): Quality **Low**,
  resolution index 1280×720 (folds to ~90% of the 800p gamescope desktop in borderless),
  render scale **75%**, **60 fps cap**, UI scale **1.10**, borderless fullscreen.
  Net render resolution ≈ 864×540 upscaled by TSR — sized for Van Gogh (RDNA2, ~half an
  Ally's GPU). `pf.HandheldPreset 2` re-applies; `pf.HandheldInfo` logs the signals.
- **Full gamepad play + pad-driven menus**: the Deck's controls present as an XInput pad
  through Steam Input, so the fixed layout and the gamepad UI cursor from
  `docs/handheld-windows.md` just work. The Deck's own touchpads/OSK complement it.
- **Auth needs no browser**: account login is an RFC 8628 device code shown as *text*
  ("Code XXXXXX — go to …") — players use their phone. Game Mode's lack of a browser
  doesn't block sign-in.
- **16:10 (1280×800)**: menus are ScaleBox width-fit and the HUD is anchor-based; FOV uses
  the engine default vertical-FOV constraint, so 16:10 shows slightly *more* vertical
  world, never letterboxes.

This branch adds the one Game-Mode papercut fix: the Website / Discord / Store / Donate
buttons attempt the browser as before, but on Deck also print the address in the status
line ("If no browser opened: …") because `LaunchURL` fails silently under gamescope.

## 2. Proton compatibility audit (what the game uses → verdict)

| Subsystem | Game usage | Under Proton |
|---|---|---|
| Rendering | D3D12 SM6 (Nanite, TSR, VSM) | VKD3D-Proton translates; **needs SM 6.6 → require Proton Experimental or GE-Proton 8+** on current SteamOS Mesa. SM5 fallback exists but Nanite content wants SM6. |
| Networking | UE `HTTP` (WinHTTP) to `api.playcombatforge.com` + raw UDP 7777 to the fleet | Both standard-supported. TLS via Proton's schannel→OpenSSL shim, works with Cloudflare certs. |
| Audio | XAudio2 (cooked SoundWaves) | FAudio — native path in Proton. Media Framework was already purged (`63ed9a5`), so no WMF risk. |
| Saves/auth | `%LOCALAPPDATA%\CombatForge` (arenas, Auth.json, Identity.json) | Maps inside the game's Proton prefix — persistent per prefix. Note: switching the compat tool remakes the prefix → fresh install GUID + re-login. |
| Browser | `LaunchURL` on 4 menu link buttons | No browser in Game Mode → status-line fallback (this branch). Login never needs it. |
| Input | XInput via Steam Input | Use the **Gamepad** template (default). Touchpad-as-mouse also works with the menus since they're real mouse UI. |
| Shader stutter | No shipped PSO cache | First session may hitch while VKD3D fills its pipeline cache; smooths out after. Acceptable for alpha; PSO precache is a later packaging task. |

**No code blockers found.** The two things that can only be proven on hardware: real-world
performance of the preset (Low/75%/800p on Van Gogh) and the SM6-on-VKD3D rendering path.

## 3. Player install guide (itch build → Deck)

Until the game is on Steam, the path is "add the itch Windows build as a non-Steam game":

1. **Desktop Mode** → install the [itch.io app](https://itch.io/app) (or download the zip
   from `thathorseslayer.itch.io/combatforge` directly) and install/extract CombatForge.
2. Steam → **Add a Non-Steam Game** → browse to `CombatForge.exe`.
3. Properties → **Compatibility** → force **Proton Experimental** (or GE-Proton 8+).
4. (Optional) Launch options: `-pfsteamdeck` — only needed if the preset/log shows
   detection missed (belt-and-braces; `SteamDeck=1` should already be set).
5. Game Mode → launch. First run applies the Deck preset automatically and signs in via
   the phone code.

Alternative: **Heroic/Lutris** users can point them at the itch page; set the same Proton
version and launch option.

Text entry (JOIN IP box, map labels): **Steam + X** opens the Deck OSK, which types into
the focused field.

## 4. On-device verification checklist (Deck)

1. Boot log: `HandheldDetect: … SteamDeckEnv='1' -> SteamDeck`, `Handheld preset applied
   (SteamDeck): quality=0 resIdx=0 resScale=75% uiScale=1.10 cap=60`.
2. Menus render un-cropped at 1280×800; pad cursor works end-to-end (boot menu → lobby →
   options → vote), Steam+X OSK types into the JOIN box.
3. Sign-in via phone code; server browser lists the Vultr fleet; join + play a full online
   match vs the live Windows build (protocol 16 — cross-play is the whole point).
4. `stat fps`: ≥40 sustained in a busy combat phase at the preset (aim: 45–60). If short,
   drop to 65% scale / lower TSR quality in `ApplyHandheldPreset`.
5. Rendering sanity vs the SM6/VKD3D path: no black/checker materials (Nanite props,
   VT textures), no missing TSR history smearing beyond the desktop norm.
6. Link buttons in Game Mode show the status-line URL fallback.
7. Battery: note W draw at the preset (target ≤ ~18W total for tolerable sessions).

## 5. Steam-proper later (not this branch)

When the Epic/Steam beta happens (docs/store-beta-plan.md): ship the **Shipping** config
(closes the `~` console), add a Steam Deck controller config + store-page Deck notes, and
consider PSO precaching for first-run smoothness. Steam Deck Verified needs: default
controller config ✅ (native pad support), OSK invocation (Steam+X works; in-game
invocation is issue #42), legible text at 800p (UI scale 1.10 ✅ — verify), and no
launcher/browser dependency ✅.
