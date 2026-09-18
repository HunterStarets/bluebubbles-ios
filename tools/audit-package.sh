#!/bin/sh
# Static audit of a built Theos package. Never touches a device.
#
#   tools/audit-package.sh packages/<name>_<version>_iphoneos-arm.deb
#
# Checks: exactly two armv7 Mach-O dylibs and two MobileSubstrate plists,
# the Settings pane bundle and its PreferenceLoader entry, nothing else in
# the payload, iOS 9.0 minimum version and SDK 9.3, the control version
# matches the file name, and neither dylib imports a forbidden symbol: compiler-lowered
# _memcpy/_memset/_strchr or any C++ exception/runtime symbol (the documented
# SpringBoard toolchain rule). The stricter per-object audit for the BB* C
# modules (no strlen, no 64-bit division helpers) lives in tools/audit-modules.sh.
# Exits non-zero on any failure and prints the SHA-256 of the package.

set -eu

PACKAGE=${1:?package path required}
THEOS=${THEOS:-$HOME/theos}
TOOLCHAIN=${THEOS_TOOLCHAIN_BIN:-$THEOS/toolchain/linux/iphone/bin}
FORBIDDEN='^(_memcpy|_memset|_strchr|___cxa_[A-Za-z_]*|__ZSt[A-Za-z0-9_]*)$'

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

sha256sum "$PACKAGE"
cd "$WORK"
ar x "$(cd - >/dev/null && cd "$(dirname "$PACKAGE")" && pwd)/$(basename "$PACKAGE")"
for archive in data.tar.*; do
    case "$archive" in
        *.xz) tar -xJf "$archive" ;;
        *.gz) tar -xzf "$archive" ;;
        *.lzma) xz -d -F lzma -k "$archive" && tar -xf data.tar ;;
        *) echo "unknown data archive $archive" >&2; exit 1 ;;
    esac
done
for archive in control.tar.*; do
    case "$archive" in
        *.xz) tar -xJf "$archive" ;;
        *.gz) tar -xzf "$archive" ;;
        *.lzma) xz -d -F lzma -k "$archive" && tar -xf control.tar ;;
    esac
done

status=0
DYLIBS=$(find . -name '*.dylib' | sort)
PLISTS=$(find . -path '*/DynamicLibraries/*.plist' | sort)
DYLIB_COUNT=$(printf '%s\n' "$DYLIBS" | grep -c . || true)
PLIST_COUNT=$(printf '%s\n' "$PLISTS" | grep -c . || true)
echo "dylibs: $DYLIB_COUNT"; printf '%s\n' "$DYLIBS"
echo "plists: $PLIST_COUNT"; printf '%s\n' "$PLISTS"
[ "$DYLIB_COUNT" -eq 2 ] || { echo "FAIL: expected two dylibs"; status=1; }
[ "$PLIST_COUNT" -eq 2 ] || { echo "FAIL: expected two plists"; status=1; }

# The Settings pane: one PreferenceLoader entry and one bundle whose
# executable is an armv7 Mach-O bundle, iOS 9.0 / SDK 9.3 like the dylibs.
PANE=./Library/PreferenceBundles/BlueBubblesiOSPrefs.bundle/BlueBubblesiOSPrefs
ENTRY=./Library/PreferenceLoader/Preferences/BlueBubblesiOS.plist
[ -f "$PANE" ] || { echo "FAIL: preference bundle executable missing"; status=1; }
[ -f "$ENTRY" ] || { echo "FAIL: PreferenceLoader entry missing"; status=1; }
[ -f ./Library/PreferenceBundles/BlueBubblesiOSPrefs.bundle/Root.plist ] ||
    { echo "FAIL: Root.plist missing"; status=1; }
if [ -f "$PANE" ]; then
    echo "== $PANE"
    file "$PANE" | grep -qE 'Mach-O armv7 (bundle|dynamically linked shared library)' || { echo "FAIL: not an armv7 Mach-O"; status=1; }
    "$TOOLCHAIN/otool" -l "$PANE" | grep -A3 'LC_VERSION_MIN_IPHONEOS' |
        grep -E 'version 9\.0$' >/dev/null || { echo "FAIL: minimum iOS is not 9.0"; status=1; }
fi
# Nothing else may ship: no stray files, no certificates, no keys.
EXTRA=$(find . -type f ! -name '*.dylib' ! -path '*/DynamicLibraries/*.plist' \
    ! -path './Library/PreferenceBundles/*' ! -path './Library/PreferenceLoader/*' \
    ! -name control ! -name 'md5sums' ! -name 'postinst' ! -name 'prerm' ! -name 'postrm' \
    ! -name 'control.tar*' ! -name 'data.tar*' ! -name 'debian-binary' | sort)
[ -z "$EXTRA" ] || { echo "FAIL: unexpected files in package:"; printf '%s\n' "$EXTRA"; status=1; }

CONTROL_VERSION=$(sed -n 's/^Version: //p' control)
CONTROL_PACKAGE=$(sed -n 's/^Package: //p' control)
EXPECTED="${CONTROL_PACKAGE}_${CONTROL_VERSION}_iphoneos-arm.deb"
if [ "$(basename "$PACKAGE")" = "$EXPECTED" ]; then
    echo "control: $CONTROL_PACKAGE $CONTROL_VERSION (matches file name)"
else
    echo "FAIL: control says $EXPECTED, file is $(basename "$PACKAGE")"; status=1
fi

for dylib in $DYLIBS; do
    echo "== $dylib"
    file "$dylib" | grep -q 'Mach-O armv7 dynamically linked shared library' ||
        { echo "FAIL: not an armv7 dylib"; status=1; }
    "$TOOLCHAIN/otool" -l "$dylib" | grep -A3 'LC_VERSION_MIN_IPHONEOS' |
        grep -E 'version 9\.0$' >/dev/null || { echo "FAIL: minimum iOS is not 9.0"; status=1; }
    "$TOOLCHAIN/otool" -l "$dylib" | grep -A3 'LC_VERSION_MIN_IPHONEOS' |
        grep -E 'sdk 9\.3$' >/dev/null || { echo "FAIL: SDK is not 9.3"; status=1; }
    BAD=$("$TOOLCHAIN/llvm-nm" -u "$dylib" | grep -E "$FORBIDDEN" || true)
    if [ -n "$BAD" ]; then
        echo "FAIL: forbidden imports:"; printf '%s\n' "$BAD"; status=1
    else
        echo "forbidden-symbol scan: clean"
    fi
    echo "objc classes: $("$TOOLCHAIN/llvm-nm" "$dylib" 2>/dev/null | grep -c '_OBJC_CLASS_\$_' || true)"
done

[ "$status" -eq 0 ] && echo "AUDIT PASSED" || echo "AUDIT FAILED"
exit "$status"
