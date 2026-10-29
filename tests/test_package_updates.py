"""Package upgrades preserve local settings and refuse unowned/modified payloads."""
import hashlib,importlib.util,json,tempfile,types,unittest
from pathlib import Path
from unittest.mock import patch
import sys
ROOT=Path(__file__).resolve().parents[1]
stubs={name:types.ModuleType(name) for name in ['prepare_addon_release','build_experimental_rtx_addon']}
stubs['prepare_addon_release'].safe_payload=lambda *_:None
stubs['build_experimental_rtx_addon'].extract_runtime=lambda *_:None
with patch.dict(sys.modules,stubs):
 spec=importlib.util.spec_from_file_location('package_player_tested',ROOT/'tools/package_player.py');package=importlib.util.module_from_spec(spec);spec.loader.exec_module(package)
def inventory(folder,files):
 (folder/'package-manifest.json').write_text(json.dumps({'files':{name:hashlib.sha256(data).hexdigest() for name,data in files.items()}}))
 for name,data in files.items():(folder/name).write_bytes(data)
class PackageUpdates(unittest.TestCase):
 def test_settings_survive_and_obsolete_owned_files_are_removed(self):
  with tempfile.TemporaryDirectory() as temp:
   base=Path(temp);player=base/'player';player.mkdir();destination=base/'dlss5-everything/runtime/media';destination.mkdir(parents=True);stage=base/'stage';stage.mkdir()
   inventory(destination,{'engine.dll':b'old','obsolete.dll':b'old'});inventory(stage,{'engine.dll':b'new'});(destination/'local-recipe.json').write_text('private settings')
   with patch.object(package,'ROOT',player):self.assertEqual(package.install(stage),destination)
   self.assertEqual((destination/'engine.dll').read_bytes(),b'new');self.assertFalse((destination/'obsolete.dll').exists());self.assertEqual((destination/'local-recipe.json').read_text(),'private settings')
 def test_locally_modified_owned_file_is_not_overwritten(self):
  with tempfile.TemporaryDirectory() as temp:
   base=Path(temp);player=base/'player';player.mkdir();destination=base/'dlss5-everything/runtime/media';destination.mkdir(parents=True);stage=base/'stage';stage.mkdir()
   inventory(destination,{'engine.dll':b'old'});inventory(stage,{'engine.dll':b'new'});(destination/'engine.dll').write_bytes(b'edited')
   with patch.object(package,'ROOT',player),self.assertRaises(RuntimeError):package.install(stage)
   self.assertEqual((destination/'engine.dll').read_bytes(),b'edited')
if __name__=='__main__':unittest.main()
