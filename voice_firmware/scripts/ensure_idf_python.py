Import("env")

import os
from pathlib import Path
import shutil
import subprocess

platformio_home = Path(
    os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")
)
python_directories = sorted(
    (platformio_home / "penv").glob(".espidf-*/bin"), reverse=True
)

for python_directory in python_directories:
    if (python_directory / "python").is_file():
        env.PrependENVPath("PATH", str(python_directory))
        break
else:
    raise RuntimeError(
        "PlatformIO's ESP-IDF Python environment is missing; "
        "run the voice target build once to install it."
    )


def build_voice_images(source, target, env):
    build_directory = Path(env.subst("$BUILD_DIR"))
    ninja = shutil.which("ninja", path=env["ENV"].get("PATH"))
    if ninja is None:
        ninja = str(platformio_home / "packages" / "tool-ninja" / "ninja")
    subprocess.run(
        [
            ninja,
            "-C",
            str(build_directory),
            "spiffs_spiffs_bin",
            "srmodels_bin",
        ],
        check=True,
        env=env["ENV"],
    )


env.Append(
    FLASH_EXTRA_IMAGES=[
        ("0x510000", "$BUILD_DIR/srmodels/srmodels.bin"),
        ("0xa1c000", "$BUILD_DIR/spiffs.bin"),
    ]
)
env.AddPreAction("upload", build_voice_images)
