# Post-build: archive every firmware.elf by its SHA256 prefix so any crash
# backtrace (which prints "ELF file SHA256: <prefix>") can be decoded later,
# even after the working tree has moved on.

import hashlib
import os
import shutil

Import("env")  # noqa: F821


def archive_elf(source, target, env):
    elf = target[0].get_abspath()
    if not os.path.exists(elf):
        return
    with open(elf, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    out_dir = os.path.join(env.subst("$PROJECT_DIR"), ".elf-archive")
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, f"{sha[:9]}.elf")
    shutil.copyfile(elf, dst)
    print(f"Archived ELF: .elf-archive/{sha[:9]}.elf")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", archive_elf)  # noqa: F821
