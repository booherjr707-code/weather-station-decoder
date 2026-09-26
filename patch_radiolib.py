# Some CC1101 clone modules report chip version 0x07, which RadioLib rejects as
# "chip not found". This adds 0x07 to the versions RadioLib accepts.
Import("env")
import os

path = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV/RadioLib/src/modules/CC1101/CC1101.cpp")
old = "(version == RADIOLIB_CC1101_VERSION_CLONE))"
new = "(version == RADIOLIB_CC1101_VERSION_CLONE) || (version == 0x07))"
if os.path.exists(path):
    text = open(path).read()
    if old in text and "version == 0x07" not in text:
        open(path, "w").write(text.replace(old, new))
        print("patch_radiolib: accepted CC1101 clone version 0x07")
