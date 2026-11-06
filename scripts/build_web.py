"""Generate minified, precompressed LittleFS assets on the build host."""
import subprocess
from pathlib import Path

Import("env")

project = Path(env.subst("$PROJECT_DIR"))
destination = Path(env.subst("$BUILD_DIR")) / "web-assets"
subprocess.run(
    ["node", str(project / "scripts" / "build_web.cjs"), "--out", str(destination)],
    cwd=project, check=True,
)
env.Replace(PROJECT_DATA_DIR=str(destination))
