# Copyright (c) 2026 Tom Chapman. All rights reserved.
#
# Trim the Bandit pack's textures for an indie game: cap every texture's Maximum Texture Size so the COOK
# produces <= CAP-res textures instead of 4K. Non-destructive + reversible (the 4K source stays in the .uasset;
# set max_texture_size back to 0 to undo). Shrinks the *packaged* build dramatically — a 4K texture capped to
# 1024 cooks to ~1/16 the pixels.
#
# Run headless (editor CLOSED):
#   "<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" D:\projects\combatforge\CombatForge.uproject
#     -run=pythonscript -script="D:\projects\combatforge\Scripts\trim_bandit_textures.py"
#
# Re-run after re-downloading the pack on another machine (e.g. the Mac) to apply the same cap there.
import unreal

CAP = 1024        # 4K -> 1K. Bump to 2048 if hero weapon/arms look soft up close.
FOLDER = "/Game/Bandits"

eal = unreal.EditorAssetLibrary
paths = eal.list_assets(FOLDER, recursive=True, include_folder=False)

capped = 0
already = 0
skipped_nontex = 0
for p in paths:
    asset = eal.load_asset(p)
    if not isinstance(asset, unreal.Texture2D):
        skipped_nontex += 1
        continue
    cur = asset.get_editor_property("max_texture_size")
    if cur == CAP:
        already += 1
        continue
    asset.set_editor_property("max_texture_size", CAP)
    eal.save_asset(p, only_if_is_dirty=False)
    capped += 1

unreal.log("trim_bandit_textures: capped %d textures to <= %d (%d already capped, %d non-textures)"
           % (capped, CAP, already, skipped_nontex))
