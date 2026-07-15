# Animation Roadmap — from "indie tell" to AAA feel

Status after the 2026-07-15 overnight pass, plus the researched recipe for the remaining tiers.
Research sources: Dev_Unallocated procedural-viewmodel writeup, Destiny GDC first-person talk,
Epic docs/forums on FSkeletonRemapping + FAnimInstanceProxy, CS:GO viewmodel constants.

## Shipped (this pass)

**First person (procedural, no assets needed) — `CombatForgeCharacter::UpdateViewmodelFeel`**
- Look-lag sway: damped springs (k≈260, ζ≈0.8) oppose camera rotation, translation + rotation channels,
  yaw-coupled roll. ADS suppresses to 30%.
- Movement bob: figure-8 (vertical at 2x lateral frequency), phase driven by speed (~2.7 Hz at jog),
  amplitude eased in/out, killed airborne, ~12% while ADS.
- Idle breathing: 0.42 Hz, ±0.14 cm — a still gun is never a screenshot.
- Fire kick: spring **velocity** impulses (back 55 cm/s, pitch-up 140 °/s, random yaw/roll) with
  underdamped settle = one visible bounce-back; accumulation clamped ~2x single shot for full-auto.
- Landing dip: impulse scaled by fall distance on the same spring stack.
- Tuning: all constants in `UpdateViewmodelFeel` / `OnFireCosmetic`. If something feels off, tweak the
  impulse magnitudes first, stiffness second.

**Third person (asset-driven via compatible skeletons)**
- The UE5 template rifle kit (`/Game/Characters`, SK_Mannequin) now plays on the Bandit bodies through
  UE 5.2+ runtime FSkeletonRemapping. `Scripts/add_compatible_skeleton.py` registered SK_Mannequin +
  HeroTPP (ASP) on `SKM_Bandit_Skeleton` (one-time per machine that cooks; asset committed).
- 8-direction rifle walk + jog (real strafe/backpedal anims picked from velocity-vs-facing, 45° buckets),
  rifle-ready idle (`MF_Rifle_Idle_ADS`), ASP `Sprint_Fwd_Rifle`, directional death reactions
  (`MM_Death_Front/Back/Left/Right`) before the body hides.
- Kill switch: `pf.ArmedAnims 0` + respawn reverts to the unarmed A_MM_* set.

## Next tier (the real engineering item): C++ anim graph

Single-node playback can't blend. The path to crossfades + aim offsets + upper/lower layering with zero
editor AnimBPs is a custom `UAnimInstance` + `FAnimInstanceProxy` owning engine anim nodes directly:

- Members on the proxy: `FAnimNode_SequencePlayer` (locomotion A/B for crossfade),
  `FAnimNode_LayeredBoneBlend` (upper body from `spine_01`, blend depth 2-4, mesh-space rotation blend),
  `FAnimNode_BlendSpacePlayer` pointed at `AO_Rifle` (aim offset: X=yaw delta, Y=pitch),
  optional `FAnimNode_Slot` for montage support.
- Wire in `FAnimInstanceProxy::Initialize` via `SetLinkNode`; evaluate the root into the output pose.
  UE5.6 note: SequencePlayer settings sit behind `GetGraphData()` accessors.
- Aim pitch without assets (do regardless): custom `FAnimNode_SkeletalControlBase` distributing remote
  view pitch across spine_01/02/03 (+neck) at weights {0.3, 0.3, 0.25, 0.15}, clamp ±60°. This single
  feature makes opponents feel alive. Template: engine's `FAnimNode_ModifyBone` (~30 lines).
- Hit reactions once the graph exists: `MM_HitReact_*` (8 directional variants already in /Game/Characters)
  as an additive layer, blend-in 0.05s / out 0.2s. Cheaper alternative that needs no graph:
  `UPhysicalAnimationComponent` below spine_01 at blend weight 0.3-0.6 + `AddImpulseAtLocation` at the
  hit bone — per-bone, per-direction reactions with zero anim assets (needs a physics asset on SKM_Body).
- Crossfade fallback if the graph slips: `FAnimationRuntime::BlendTwoPosesTogether` with a 0.15-0.25s
  alpha fixes the pop between directional sequences.

## Fab shopping list (Tom)

**Important: the three Fab packs downloaded 2026-07-14 imported WITHOUT their animations** —
`Character_Mobility_Animation_Mocap_Pack`, `Modern_Gun_Shooting_Mocap_Pack`, and `Standing_with_pistol`
contain only meshes/skeletons/materials on disk. Re-add them from the Fab library and make sure the
AnimSequences are included this time (they're also on their own non-mannequin skeleton, so the template
kit remains the better source — only bother if something below is missing).

What would actually move the needle, in priority order:
1. **Rifle crouch locomotion on a UE5-Mannequin skeleton** — the template kit has none; ASP's crouch set
   is UE4-skeleton and hip-only. Search Fab: "rifle crouch locomotion mannequin".
2. **Reload + equip full-body TP anims** are already in the kit (`MM_Rifle_Reload/Equip`) — nothing to buy;
   they wire up once the montage slot exists (anim-graph tier).
3. **First-person ARMS (true viewmodel with hands)** — the single biggest remaining FP upgrade. Search:
   "FPS arms pack mannequin" / "fp arms rifle animations". Everything procedural transfers to real arms.
4. **Turn-in-place + lean additives** (mannequin) — polish tier, after the anim graph.

Skip: generic mobility mocap on custom skeletons (retarget hassle, template kit already covers it).
