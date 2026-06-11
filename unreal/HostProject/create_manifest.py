"""Create /AetherForge/DA_AetherForgeManifest with the four contract categories
mapped to engine basic shapes (placeholder art). Run headless:
  UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script=<this file>
"""
import unreal

ASSET_NAME = "DA_AetherForgeManifest"
PACKAGE_PATH = "/AetherForge"
ASSET_PATH = f"{PACKAGE_PATH}/{ASSET_NAME}"

# (key, mesh, footprint_cm, instanceable, ground_snap, display)
CATEGORIES = [
    ("deciduous_tree", "/Engine/BasicShapes/Cylinder.Cylinder", 150.0, True,  True, "Deciduous Tree (placeholder)"),
    ("shrub",          "/Engine/BasicShapes/Sphere.Sphere",      60.0, True,  True, "Shrub (placeholder)"),
    ("boulder",        "/Engine/BasicShapes/Cube.Cube",          90.0, True,  True, "Boulder (placeholder)"),
    ("dog_large",      "/Engine/BasicShapes/Cone.Cone",          80.0, False, True, "Large Dog (placeholder)"),
    ("deer",           "/Engine/BasicShapes/Cone.Cone",         100.0, False, True, "Deer (placeholder)"),
]

# Idempotent: modify the existing asset in place; create only if absent.
# Commandlet runs have not scanned plugin content into the Asset Registry yet,
# so force a scan, and load via load_object (direct LoadObject, no registry).
unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([PACKAGE_PATH], force_rescan=True)
manifest = unreal.load_object(None, f"{ASSET_PATH}.{ASSET_NAME}")
if manifest is None:
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.AetherForgeManifest)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    manifest = asset_tools.create_asset(ASSET_NAME, PACKAGE_PATH, unreal.AetherForgeManifest, factory)
assert manifest is not None, "could not load or create the manifest asset"

entries = unreal.Map(str, unreal.AetherForgeManifestAssetEntry)
for key, mesh, radius, instanceable, ground_snap, display in CATEGORIES:
    entry = unreal.AetherForgeManifestAssetEntry()
    entry.set_editor_property("asset_path", unreal.SoftObjectPath(mesh))
    entry.set_editor_property("footprint_radius", radius)
    entry.set_editor_property("instanceable", instanceable)
    entry.set_editor_property("ground_snap", ground_snap)
    entry.set_editor_property("display_name", display)
    entries[key] = entry

manifest.set_editor_property("entries", entries)
ok = unreal.EditorAssetLibrary.save_asset(ASSET_PATH, only_if_is_dirty=False)
assert ok, "save_asset failed"
print(f"AETHERFORGE_MANIFEST_OK: {ASSET_PATH} with {len(CATEGORIES)} categories")
