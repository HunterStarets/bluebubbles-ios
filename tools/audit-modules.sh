#!/bin/sh
# Compile every BB* C module as an optimized armv7/iOS 9.0 object with the
# iPhoneOS 9.3 SDK at -Os, -O2, and -O3 and fail if any undefined symbol is a
# libc call the compiler lowered from a manual loop, a C++ runtime symbol, or
# a 64-bit division helper. Never touches a device.
#
#   tools/audit-modules.sh

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
THEOS=${THEOS:-$HOME/theos}
TOOLCHAIN=${THEOS_TOOLCHAIN_BIN:-$THEOS/toolchain/linux/iphone/bin}
SDK=${THEOS_IOS_SDK:-$THEOS/sdks/iPhoneOS9.3.sdk}
FORBIDDEN='^(_memcpy|_memset|_memmove|_strchr|_strlen|_strcmp|___cxa_[A-Za-z_]*|__ZSt[A-Za-z0-9_]*|___aeabi_(u?l?div|uldivmod|ldivmod)[A-Za-z0-9_]*|___udivmoddi4|___divdi3|___udivdi3)$'
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
status=0
for module in "$ROOT"/common/BB*.c; do
    name=$(basename "$module" .c)
    for opt in -Os -O2 -O3; do
        object="$WORK/$name$opt.o"
        "$TOOLCHAIN/clang" -target armv7-apple-ios9.0 -isysroot "$SDK" \
            -miphoneos-version-min=9.0 "$opt" -fno-builtin -std=c99 -Wall -Wextra -Werror \
            -I"$ROOT/include" -c "$module" -o "$object" 2>&1 | grep -v 'tbd\|simulator' || true
        [ -f "$object" ] || { echo "FAIL: $name $opt did not compile"; status=1; continue; }
        external=$("$TOOLCHAIN/llvm-nm" -u "$object" | grep -v '^_bb_' | tr '\n' ' ')
        bad=$("$TOOLCHAIN/llvm-nm" -u "$object" | grep -E "$FORBIDDEN" || true)
        [ "$opt" = "-Os" ] && echo "== $name: ${external:-<none>}"
        if [ -n "$bad" ]; then echo "FAIL: $name $opt imports $bad"; status=1; fi
    done
done
[ "$status" -eq 0 ] && echo "MODULE AUDIT PASSED" || echo "MODULE AUDIT FAILED"
exit "$status"
