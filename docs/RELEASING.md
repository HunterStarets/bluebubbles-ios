# Releasing

1. Bump `Version:` in `control` and add the section to `CHANGELOG.md`;
   keep `docs/COMPATIBILITY.md` in step with what changed.
2. Host suite and sanitizers: `cd tests && make check && make sanitize`.
3. Build and audit:
   ```sh
   make clean package FINALPACKAGE=1
   tools/audit-package.sh packages/com.hunterstarets.bluebubbles-ios_<version>_iphoneos-arm.deb
   tools/audit-modules.sh
   ```
4. Install on the reference device (`docs/TESTING.md`), run the probe
   (17/17), and do the manual send checks against your own number.
5. Tag `v<version>`, create the GitHub release, attach the `.deb` and its
   `sha256sum` output. `packages/` is not tracked; the release is the
   only place the binary lives.
