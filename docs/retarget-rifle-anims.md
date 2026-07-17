# Fix the bot "gun at the forehead" — the right way (IK-retarget the low-ready anims)

*Why:* the Shooter Rifle Animations pack's low-ready pose is exactly what we want (measured: gun
sits ~25 uu **below** the head = chest carry, no forehead, hands on the gun). Its **only** problem
is that it's authored on a UE4-mannequin skeleton and the runtime name-based remap onto the Bandit
**stretches the body**. A proper IK-retarget bakes the anims onto the Bandit's own skeleton at the
Bandit's proportions → the stretch is gone and the good low-ready hold survives.

There are two ways to produce the retargeted anims. **Do either one, then tell me — I flip the code
(repoint the anim paths + default `pf.ArmedAnims 1`) and build.**

---

## Option A — let me do it headlessly (least effort for you)
Just **close the editor** and tell me. I run `Scripts/retarget_rifle_to_bandit.py`, which bakes the
retargeted anims into `/Game/RifleAnims/Animations/Bandit_Retargeted/` automatically. Then I repoint
+ build, you reopen and eyeball it. If UE 5.6's auto-generation API doesn't cooperate, I'll tell you
and we use Option B.

## Option B — you do it in the editor (~10–15 min, most reliable)
The one step a human eye helps with is the base-pose match, so this is the sure path.

1. **Source IK Rig:** Content Browser → `/Game/RifleAnims/ShowcaseAssets/Meshes/Mannequin/SK_Mannequin`
   → right-click → **Create → IK Rig**. Accept the auto-generated biped chains. Save as
   `IK_RifleSrc` in `/Game/RifleAnims/Animations/Bandit_Retargeted/`.
2. **Target IK Rig:** same on `/Game/Bandits/Mesh/Body/SKM_Body` → **Create → IK Rig** →
   save as `IK_Bandit` in the same folder. (Both are Mannequin-family — pelvis/spine_/upperarm_l/
   hand_r — so the auto-chains line up by name.)
3. **IK Retargeter:** right-click → **Create → IK Retargeter**. Set **Source = IK_RifleSrc**,
   **Target = IK_Bandit**. The chain map auto-fills by name. In the viewport, check the two
   skeletons overlap in a similar pose; if the Bandit's hips/feet float, use **Edit Pose** and nudge
   until they sit right (this is the human-eye step).
4. **Bake:** in the retargeter's **Asset Browser** panel, select these 14 sequences from
   `/Game/RifleAnims/Animations/BlendSpaces/Standing_IdleWalkJogRun/`:
   `AS_Rifle_Idle, AS_Rifle_RunFwd, AS_Rifle_WalkFwd, AS_Rifle_WalkRight, AS_Rifle_WalkBwdRight,
   AS_Rifle_WalkBwd, AS_Rifle_WalkBwdLeft, AS_Rifle_WalkLeft, AS_Rifle_JogFwd, AS_Rifle_JogRight,
   AS_Rifle_JogBwdRight, AS_Rifle_JogBwd, AS_Rifle_JogBwdLeft, AS_Rifle_JogLeft`
   → right-click → **Export Selected Animations** (a.k.a. Duplicate and Retarget) →
   **output folder `/Game/RifleAnims/Animations/Bandit_Retargeted/`, suffix `_Bandit`**. Save All.

**Use that exact folder + `_Bandit` suffix** — my code repoint is written to match it, so once the
assets exist it's one build and done.

---

## After either option (my part, no editor needed)
- Repoint the 14 `AS_Rifle_*` paths in `CombatForgeCharacter.cpp` (idle + `WalkDirPaths[8]` +
  `JogDirPaths[8]` + run) to `.../Bandit_Retargeted/AS_Rifle_*_Bandit`.
- Flip `pf.ArmedAnims` default `0 → 1`.
- Build both targets, then you PIE-verify: no stretch, rifle at chest/low-ready (not the face),
  hands on the gun, firing bots still shoot from the barrel.
