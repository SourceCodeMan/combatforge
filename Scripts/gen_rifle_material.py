import unreal

TEX = "/Game/Weapons/Rifle/Textures"
d   = unreal.load_asset(TEX + "/T_Rifle_D")
n   = unreal.load_asset(TEX + "/T_Rifle_Combined_N")
orm = unreal.load_asset(TEX + "/T_Rifle_AORM")
print("PF_RIFLE tex loaded: d=%s n=%s orm=%s" % (bool(d), bool(n), bool(orm)))

tools = unreal.AssetToolsHelpers.get_asset_tools()
mel = unreal.MaterialEditingLibrary
ST = unreal.MaterialSamplerType
MP = unreal.MaterialProperty

# Fresh create (delete if it already exists so re-runs are clean)
if unreal.EditorAssetLibrary.does_asset_exist("/Game/Weapons/Rifle/M_PF_Rifle"):
    unreal.EditorAssetLibrary.delete_asset("/Game/Weapons/Rifle/M_PF_Rifle")

mat = tools.create_asset("M_PF_Rifle", "/Game/Weapons/Rifle", unreal.Material, unreal.MaterialFactoryNew())

def tex_node(tex, x, y, sampler):
    node = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSample, x, y)
    node.set_editor_property("texture", tex)
    node.set_editor_property("sampler_type", sampler)
    return node

bc = tex_node(d,   -520, -260, ST.SAMPLERTYPE_COLOR)
mel.connect_material_property(bc, "RGB", MP.MP_BASE_COLOR)

nm = tex_node(n,   -520,   40, ST.SAMPLERTYPE_NORMAL)
mel.connect_material_property(nm, "RGB", MP.MP_NORMAL)

om = tex_node(orm, -520,  340, ST.SAMPLERTYPE_MASKS)   # AO=R, Rough=G, Metal=B (packed mask texture)
mel.connect_material_property(om, "R", MP.MP_AMBIENT_OCCLUSION)
mel.connect_material_property(om, "G", MP.MP_ROUGHNESS)
mel.connect_material_property(om, "B", MP.MP_METALLIC)

mel.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset("/Game/Weapons/Rifle/M_PF_Rifle")
print("PF_RIFLE_MATERIAL_DONE exists=%s" % unreal.EditorAssetLibrary.does_asset_exist("/Game/Weapons/Rifle/M_PF_Rifle"))
