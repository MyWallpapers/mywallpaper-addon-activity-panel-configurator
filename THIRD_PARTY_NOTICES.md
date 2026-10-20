# Third party notices

nlohmann/json 3.12.0, copyright Niels Lohmann, MIT license. Exact source and
notice hashes are recorded in native/dependencies.lock.json. License text:
third_party/nlohmann/LICENSE.MIT (packaged as notices/nlohmann-json-LICENSE.MIT).

No Windhawk, shell hook, proprietary Native Engine code or SDK implementation is
linked or copied. The public MyWallpaper SDK is used read-only to validate the
manifest; its exact compiled schema files are checked against sdk-public.lock.json.
The process-v2 v5 wire contract is implemented locally from the public contract.

The A18 audio-system v1 schema, examples and README are copied byte-for-byte
from commit 44a2e0f642bf0100e9333300a2240ea653d90a84, copyright (c) 2026 Audio
System Configurator contributors, MIT. Their hashes are pinned in
contracts/audio-system/v1/provenance.json. The schema is embedded in the native
consumer; its MIT notice is packaged as notices/a18-audio-system-LICENSE.MIT.
No provider implementation or test/build runner is copied.
