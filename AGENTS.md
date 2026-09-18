# Working on BlueBubbles iOS

Guidance for anyone (or any coding agent) contributing. Device-specific
values (the test device's address, its password, the owner's own test
number) never belong in this file or in any commit; keep them in
`AGENTS.local.md`, which is gitignored.

## What this is

An unofficial BlueBubbles server for jailbroken iOS 9 (armv7, iOS 9.0–9.3.5).
Two dylibs: `BBServer` in SpringBoard (HTTP/Socket.IO server built from the
Foundation-free C modules in `common/`) and `BBBridge` in MobileSMS
(IMCore/ChatKit access), joined by a typed IPC contract. Read
`docs/ARCHITECTURE.md` first, then `docs/BUILDING.md` for the toolchain
rules.

## Commands

```sh
cd tests && make clean && make check && cd ..          # host tests (C + Node)
export THEOS=~/theos
make clean package FINALPACKAGE=1                       # armv7 package
tools/audit-package.sh packages/*.deb                   # layout + symbol audit
tools/audit-modules.sh                                  # per-module lowering audit
```

A change is not done until the host suite, both audits, and (for anything
touching the transport or bridge) the device probe pass.

## Rules that are not negotiable

- **Never fake a feature iOS 9 lacks.** Reactions, effects, edit, unsend,
  replies, mentions, Live Photos: reject with a clear `400`, never
  pretend. `docs/COMPATIBILITY.md` is the source of truth for what is
  possible.
- **No `memcpy`/`memset`/`strchr`/`strlen`/`strcmp` may be linked** in
  either dylib, no C++ exceptions, no `@finally`. Copy with volatile byte
  loops; never assign or block-capture large structs. The audits enforce
  it.
- **Every private selector is called dynamically** with a signature check,
  on the MobileSMS main queue, inside `@try/@catch`.
- **Diagnostics stay count-only.** `bb_trace` takes an enum. Nothing that
  can hold an address, name, text, guid, or credential goes in a log.
- **Sends are real.** Test sends go only to the tester's own number or
  address, one at a time, with a person directing. Never batch or script
  sends. Once a send has been dispatched, the reply must never be an error
  (the client would retry and double-send).
- **The probe is read-only.** `tests/live-bluebubbles-probe.js` must never
  print private values or perform an action; its POSTs must be ones
  validation refuses.
- **Nobody runs device commands but the operator.** Print the commands;
  the person with the device runs them.

## Conventions

- Prefix everything `BB`/`bb_`. C modules are bounded, allocation-free,
  and host-tested with sanitizers; add a test with every change.
- Reply shapes come from `BBSerialize`; do not hand-write JSON for
  responses.
- Keep `docs/COMPATIBILITY.md` and `CHANGELOG.md` in step with behavior.
- Commit messages describe the change; no tool attribution lines.
