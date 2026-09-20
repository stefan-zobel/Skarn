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
cmake --build /tmp/skarn-release/build --target skarnvm skarn_lsp -- -j$(sysctl -n hw.logicalcpu)
```

Confirm that both binaries exist and are executable before continuing.

---

### 2. Run the test suite

```bash
ctest --test-dir /tmp/skarn-release/build --output-on-failure
```

Expected totals (Release, arm64) — these must match the Windows numbers in the
release notes:

```
vm_tests               98 passed, 0 failed  (checks: 134 passed, 0 failed)
vm_fault_probe          1 passed, 0 failed
static_compiler_tests  <N> passed, 0 failed
```

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
- [ ] All three ctest entries green, totals match Windows
- [ ] Archive unpacks to a single `Skarn-<version>-macos-arm64/` folder
- [ ] Executable bit set on `skarnvm` and `skarn_lsp` (`.tar.gz` preserves it; `.zip` does not)
- [ ] `README.txt` inside the archive is the macOS-specific one (Gatekeeper note, `./skarnvm` not `static_vmrun.exe`)
- [ ] `.vsix` is the same file as in the Windows zip (identical SHA256)
- [ ] `.sha256` matches `shasum -a 256` output
- [ ] Release title no longer says `windows-x64`
- [ ] Release description covers both platforms
