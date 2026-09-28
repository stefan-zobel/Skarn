## Creating a macOS / Apple Silicon release

These steps produce `Skarn-<version>-macos-arm64.tar.gz` and attach it to the
existing GitHub release alongside the Windows zip.

### Prerequisites

- `gh` authenticated as a collaborator on `stefan-zobel/Skarn`
- Xcode Command Line Tools and CMake 3.21+
- The Windows release already published (provides the platform-independent `.vsix`)

---

### 1. Build from the release tag

Never build from a branch tip — master may have commits that are not part of
the tagged release.

```bash
git fetch --tags
git worktree add /tmp/skarn-release <tag>      # e.g. 0.2.0
cmake -B /tmp/skarn-release/build -S /tmp/skarn-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/skarn-release/build -- -j$(sysctl -n hw.logicalcpu)
```

This builds every target, the test suites included, which step 2 runs. Confirm that `skarnvm` and
`skarn_lsp` exist and are executable before continuing.

---

### 2. Run the test suite

```bash
ctest --test-dir /tmp/skarn-release/build --output-on-failure
```

All thirteen entries must pass: the four suites (`vm_tests`, `vm_fault_probe`, `static_compiler_tests`,
`skarn_lsp_selftest`) and the nine Python gates (`doc_claims`, `doc_examples`, `examples`, `doc_links`,
`doc_anchors`, `demos`, `stdlib_reference`, `highlighters`, `live_output`, which need Python 3.9 or newer).
Expected totals for 0.4.0, in ctest's order — the Windows Release run produces exactly these, and the
arm64 run must match:

```
vm_tests                146 passed, 0 failed  (checks: 187 passed, 0 failed)
vm_fault_probe            2 passed, 0 failed
static_compiler_tests  3715 passed, 0 failed
skarn_lsp_selftest      241 passed, 0 failed
doc_claims              288 passed, 0 failed  (288 claims)  (21 support modules skipped)
doc_examples            243 passed, 4 check-only, 0 ignored, 0 failed
examples                 13 passed, 0 failed
doc_links               all anchors resolve — once per guide, so four times
doc_anchors             every cited section resolves
demos                    39 passed, 0 failed  (35 entry programs, 4 self-tests)
stdlib_reference        601 documented, 57 internal natives, 1 allowed undocumented, 0 missing, 0 unknown
highlighters              0 problems
live_output               5 passed, 0 failed
```

The chat server's self-test runs inside `demos` and prints `selftest: 24 steps ok`.

`--output-on-failure` prints these lines only for a failing entry; `ctest -V` prints them all. The whole
set takes about three minutes sequentially, of which `static_compiler_tests` is roughly 140 s, so `-j`
saves little.

Do not publish if anything is red.

---

### 3. Get the VSIX

Download the `.vsix` from the existing GitHub release (it is platform-independent):

```bash
gh release download <tag> --repo stefan-zobel/Skarn \
    --pattern "*.vsix" --dir /tmp/skarn-assets
```

---

### 4. Assemble the archive

```bash
STAGE=/tmp/Skarn-<version>-macos-arm64

mkdir "$STAGE"
cp /tmp/skarn-release/build/skarnvm   "$STAGE/"
cp /tmp/skarn-release/build/skarn_lsp "$STAGE/"
cp -R /tmp/skarn-release/examples     "$STAGE/"
cp -R /tmp/skarn-release/demo         "$STAGE/"
cp /tmp/skarn-release/LICENSE         "$STAGE/"
cp /tmp/skarn-assets/skarn-language-<version>.vsix "$STAGE/"
cp README-<version>-macos.txt         "$STAGE/README.txt"
```

`README-<version>-macos.txt` lives in the repo root and becomes `README.txt`
inside the archive. It documents the Gatekeeper quarantine workaround, the
macOS version requirement (15 / Sequoia or newer, Apple Silicon), quick-start
instructions and VS Code setup.

Then pack:

```bash
cd /tmp
tar czf Skarn-<version>-macos-arm64.tar.gz Skarn-<version>-macos-arm64/
shasum -a 256 Skarn-<version>-macos-arm64.tar.gz
```

Write the SHA256 line in the same format as the Windows file
(`<hash> *<filename>`) to `Skarn-<version>-macos-arm64.tar.gz.sha256`.

---

### 5. Upload assets to the existing release

```bash
gh release upload <tag> \
    Skarn-<version>-macos-arm64.tar.gz \
    Skarn-<version>-macos-arm64.tar.gz.sha256 \
    --repo stefan-zobel/Skarn
```

---

### 6. Update the release title and description

The release title should not mention a single platform. Rename it to
`Skarn <version>` if it still says `Skarn-<version>-windows-x64`.

The release description lives in `release-<version>.md` in the repo root.
It covers both platforms, lists both downloads, and must not contain any
sentence saying the macOS port is not part of the release.

```bash
gh release edit <tag> \
    --repo stefan-zobel/Skarn \
    --title "Skarn <version>" \
    --notes-file release-<version>.md
```

---

### 7. Clean up

```bash
git worktree remove /tmp/skarn-release
```

---

### Checklist

- [ ] Built from the exact release tag, not a branch tip
- [ ] All thirteen ctest entries green, totals match Windows
- [ ] Archive unpacks to a single `Skarn-<version>-macos-arm64/` folder
- [ ] Executable bit set on `skarnvm` and `skarn_lsp` (`.tar.gz` preserves it; `.zip` does not)
- [ ] `README.txt` inside the archive is the macOS-specific one (Gatekeeper note, `./skarnvm` rather than `skarnvm.exe`)
- [ ] `.vsix` is the same file as in the Windows zip (identical SHA256)
- [ ] `.sha256` matches `shasum -a 256` output
- [ ] Release title no longer says `windows-x64`
- [ ] Release description covers both platforms
