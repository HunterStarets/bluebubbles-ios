# Building

## Toolchain

- [Theos](https://theos.dev) with the **iPhoneOS 9.3 SDK** in
  `$THEOS/sdks/iPhoneOS9.3.sdk`.
- The Linux (or macOS) iOS toolchain Theos installs; clang 11-era is fine.
- Theos headers for `AppSupport` (CPDistributedMessagingCenter) and
  `rocketbootstrap` under `$THEOS/vendor/include`, plus `librocketbootstrap`
  and `libcommonCrypto` stubs in `$THEOS/sdks/iPhoneOS9.3.sdk/usr/lib/system`.
  A stock Theos install with the 9.3 SDK provides all of them.
- Node.js 18+ and a host C compiler for the tests.

```sh
export THEOS=~/theos
make clean package FINALPACKAGE=1
```

The package lands in `packages/`. `control` is the single source of the
version; the Makefile injects it as `BB_BUILD_VERSION`, which every
diagnostic line carries.

## Audits

Two scripts keep the build honest. Run both before publishing a package:

```sh
tools/audit-package.sh packages/com.hunterstarets.bluebubbles-ios_*.deb
tools/audit-modules.sh
```

`audit-package.sh` unpacks the `.deb` (with `ar`/`tar`; no `dpkg-deb`
needed) and checks: the expected dylibs and plists, armv7 Mach-O with iOS
9.0 minimum and SDK 9.3, the control version matching the file name, and
that neither dylib imports a forbidden symbol. `audit-modules.sh` compiles
every `common/BB*.c` module at `-Os`, `-O2`, and `-O3` and fails if the
compiler lowered a loop into `memcpy`/`memset`/`strchr`/`strlen`/`strcmp`,
emitted a C++ runtime symbol, or a 64-bit division helper.

## The rules that bite on this SDK

These are the reasons the code looks the way it does:

- **The SpringBoard dylib cannot import `_memcpy`, `_memset`, `_strchr`.**
  With `-fno-builtin` and this toolchain, an ordinary struct assignment or a
  byte loop that clang recognises becomes a call to a libc symbol the link
  step cannot resolve. Every C module copies and zeroes with `volatile`
  byte loops, and Objective-C code never assigns large structs or captures
  them in blocks (heap-box them and capture the pointer;
  `BBRQCopyArguments` is the pattern).
- **No C++ exceptions or `@finally` in either dylib.** They pull in
  `__cxa_*`/`std::terminate` which do not link here. Objective-C
  `@try/@catch` is fine in the MobileSMS bridge and is used around every
  private-API call.
- **The MobileSMS bridge cannot link `_strcmp`/`_strlen` either.** Use
  `NSString` comparisons or the C modules' own helpers.
- **Every private selector is called dynamically** through
  `respondsToSelector:` plus an `NSMethodSignature` check of the return and
  argument types (`BBRQObject`, `BBRQObjectWithArgument`, …). A missing or
  differently-typed selector returns nil, never crashes.
- **All IMCore/ChatKit access is on the MobileSMS main queue.**

If a build fails at link time with `Undefined symbols … _memcpy`, look for a
new struct copy, a block capturing a struct, or a `memset` the compiler
synthesised, and replace it with the volatile-loop or heap-copy pattern.

## Layout

| Path | What |
|---|---|
| `common/`, `include/` | Foundation-free C modules: HTTP parser, WebSocket, Engine.IO/Socket.IO, JSON, multipart, router, connection state, request table, events, serializers, trace |
| `BBServer/` | SpringBoard side: `BBServerTransport` (CFSocket/CFStream owner, IPC dispatch, upload staging), IPC center, launch hook |
| `BBBridge/` | MobileSMS side: `BBBridgeRequests` (every operation), hooks that emit events |
| `Preferences/` | The Settings pane bundle |
| `tests/` | Host tests: C suites, the Node fixture server and contract test, the read-only device probe |
| `tools/` | Audit scripts |
| `docs/` | This documentation |
