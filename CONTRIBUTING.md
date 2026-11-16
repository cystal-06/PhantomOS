# Contributing to PhantomOS

PhantomOS follows Linux kernel & Buildroot development conventions for branching, commits, and verification.

## Branching Model (Gitflow)

- **`main`**: Release-ready, tagged builds (`v0.1.0`, `v0.2.0`, ...). Only receives tested merges from `develop` or critical hotfixes.
- **`develop`**: Active integration branch. Every merged feature or fix must build cleanly and pass QEMU verification.
- **`feature/<scope>`** / **`fix/<scope>`**: Branched from `develop` for each logical subsystem change (e.g., `feature/jlink-jre-strip`, `fix/timezone-and-env`), merged back via `--no-ff` or Pull Request, and deleted after merge.

## Commit Standards

1. **Conventional Commits:** `<type>(<scope>): <imperative summary>`
   - Types: `feat`, `fix`, `refactor`, `test`, `docs`, `chore`, `build`
   - Scopes: `config`, `overlayfs`, `rootfs`, `auth`, `login`, `monitor`, `desktop`, `scripts`
2. **Developer Certificate of Origin (DCO):** Sign every commit with `git commit -s` (`Signed-off-by: Name <email>`).
3. **Atomic Changes:** Keep one logical change per commit so regressions can be bisected cleanly with `git bisect`.

## Verification Checklist Before Merge

```bash
# 1. Incremental rootfs / OS build
./scripts/update-rootfs.sh   # or ./scripts/build-os.sh when defconfig/kernel changes

# 2. Stateless, auth, sudo & headless profile smoke test
./scripts/test-stateless.sh

# 3. Graphical Display Manager & Desktop smoke test
./scripts/run-qemu.sh
```
