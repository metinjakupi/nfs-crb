#!/usr/bin/env python3
"""Export Carbon's existing web preferences for its start page."""
import json
from pathlib import Path
import sys
import tomllib

root = Path(__file__).resolve().parents[1]
mod = tomllib.loads((root / "source/mods/core/nfsc/mod.toml").read_text())
rows = [
    dict(id="host.display/frame_limit", label="Frame rate", group="display", kind="int", min=0, max=3, step=1, default=2,
         choices=["Original", "40 FPS", "60 FPS", "120 FPS (experimental)"]),
    dict(id="host.display/performance_overlay", label="Frame counter", group="display", kind="int", min=0, max=2, step=1, default=2,
         choices=["Off", "FPS", "FPS and graph"]),
    dict(id="host.display/texture_filtering", label="Texture filtering", group="display", kind="int", min=0, max=4, step=1, default=3,
         choices=["Original", "Trilinear", "4× anisotropic", "8× anisotropic", "16× anisotropic"]),
]
for key, setting in mod["settings"].items():
    # Match the web menu's public controls; diagnostic switches are not player settings.
    if setting.get("group") == "diagnostics":
        continue
    boolean = setting["type"] == "bool"
    group = {"graphics": "display", "controls": "controls"}.get(setting.get("group"), "game")
    rows.append(dict(id=f"{mod['id']}/{key}", label=setting.get("label", key), group=group,
                     kind="bool" if boolean else "int", min=setting.get("min", 0),
                     max=setting.get("max", 1), step=setting.get("step", 1),
                     default=int(setting["default"]), choices=setting.get("choices", "").split("|") if setting.get("choices") else []))
out = Path(sys.argv[1])
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps({"settings": rows}, indent=2) + "\n")
