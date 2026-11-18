THEOS_PACKAGE_SCHEME = rootless
ARCHS = arm64 arm64e
TARGET = iphone:clang:latest:16.4
INSTALL_TARGET_PROCESSES = SpringBoard

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = MohsenTouchGuard
# v0.2.16: ObjC guard again (Tweak.x, v0.2.3 feature set) — built on
# macOS with Apple clang via GitHub Actions (see .github/workflows).
# Apple clang PAC-signs ObjC class pointers (isa/data/superclass),
# which the Linux L1ghtmann toolchain never did — that is why every
# L1ghtmann ObjC build crashed SpringBoard in libobjc readClass
# (docs/READCLASS-CRASH.md). The CI workflow verifies the built
# dylib's fixups with tools/minclass/check_auth.py before publishing.
# Tweak.c (pure-C guard, v0.2.15) is kept as a fallback for local
# Linux builds. Compile with -fblocks for the CTCallCenter handler
# and dispatch blocks.
MohsenTouchGuard_FILES = Tweak.x
MohsenTouchGuard_CFLAGS = -fblocks -fobjc-arc
MohsenTouchGuard_FRAMEWORKS = Foundation CoreFoundation

include $(THEOS_MAKE_PATH)/tweak.mk

SUBPROJECTS += preferences
include $(THEOS_MAKE_PATH)/aggregate.mk
