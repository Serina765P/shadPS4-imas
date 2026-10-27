# Upstream + CI reconnaissance — VK_ERROR_DEVICE_LOST in the Idolmaster fork

Task: `task-3`. Scope: read-only reconnaissance. No source edits, no branch creation, no pushes.
Reported by: teammate `upstream-ci-recon`.

Local facts used below:

| Item | Value |
| --- | --- |
| Repo | `C:\Users\Serina\Documents\ChatGPT\shadps4` |
| HEAD | `14c39880` on `release` (= `origin/release`, `origin/HEAD`) |
| Fork remote | `origin` = `https://github.com/Serina765P/shadPS4-imas.git` |
| Upstream remote | `upstream` = `https://github.com/shadps4-emu/shadPS4.git` |
| Merge-base (fork point) | `dc5a33d6` 2026-07-26 `avplayer: improve sync between audio and video (#4761)` |
| `upstream/main` after fetch | `3eecea94` 2026-10-26 `net: answer PS4 device discovery on UDP 987 (#5438)` |
| Commits in `upstream/main` not in HEAD | **424**, of which **125** touch `src/video_core`, `src/core/memory.cpp`, `src/core/libraries/avplayer`, `src/core/libraries/ajm` |
| Assertion under investigation | `src/video_core/renderer_vulkan/vk_scheduler.cpp:195` — `ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit")` |

---

## A. Upstream / known-issue reconnaissance

### A.0 Network note (important, cost me two failed attempts)

Plain `git fetch` **fails** on this machine:

```
fatal: unable to access 'https://github.com/shadps4-emu/shadPS4.git/':
       schannel: failed to receive handshake, SSL/TLS connection failed
```

The failure is HTTP/2-specific, not a firewall. `curl` and `git` with HTTP/1.1 both work:

```powershell
git -c http.version=HTTP/1.1 fetch upstream main --prune   # works, EXIT=0
git -c http.version=HTTP/1.1 fetch origin --prune          # works, EXIT=0
```

Both remotes are now up to date (`upstream/main` = `3eecea94`, `origin/release` = `14c39880`).
Every `git` network command in this document therefore carries `-c http.version=HTTP/1.1`.
The partial clone stayed partial (`blob:none`); the fetch moved only refs/trees.

### A.1 The highest-value finding: this fork diverges from upstream at three of the four `UpdateImage` call sites

`upstream/main` still performs the CPU→GPU image refresh at four points:

| File | Site | Upstream `3eecea94` | Fork `14c39880` |
| --- | --- | --- | --- |
| `src/video_core/texture_cache/texture_cache.cpp` | `FindRenderTarget` | `UpdateImage(image_id);` | **call removed** |
| `src/video_core/texture_cache/texture_cache.cpp` | `FindDepthTarget` | `UpdateImage(image_id);` | **call removed** |
| `src/video_core/texture_cache/texture_cache.cpp` | (third site) | `UpdateImage(image_id);` | unchanged |
| `src/video_core/renderer_vulkan/vk_rasterizer.cpp` | `Rasterizer::BeginRendering` | `texture_cache.UpdateImage(image_id);` | **call removed** |

Those three removals are fork commits `005f24b3` (message: *"skip UpdateImage for render targets to avoid breaking
render passes"*) plus the same hunks re-applied in `8d98d6c0`. The reasoning recorded in the comment
("render targets are GPU-written, don't need CPU upload") ignores that `UpdateImage` is also what picks up
*guest-CPU-written* content for an image that the GPU last touched — and that `UpdateImage → RefreshImage`
is the only path that issues the cache-invalidate/barrier sequence for such an image.

The second fork-only change is `b3d75746` / `8d98d6c0`: a new `MemoryManager::InvalidateMemoryAsync` that
defers invalidation to the rasterizer instead of doing the synchronous wait
(`src/core/memory.cpp`, `buffer_cache.*`, `vk_rasterizer.*`, `avplayer_source.cpp`).
Both changes are in the "GPU-side hazard" area; either is a plausible producer of a hang/TDR that surfaces as
`eErrorDeviceLost` on the next `vkQueueSubmit`.

**Verdict:** the fork's own `release` branch is a stronger suspect than any missing upstream commit.
A one-commit revert experiment (`git revert 8d98d6c0`) is the cheapest A/B the team can build on CI.

### A.2 Upstream commits not in HEAD that touch the hot paths

125 commits. The ones that plausibly relate to a device lost on submit, newest first
(all are **merged upstream**, none are in the fork):

| Commit | Subject | Why it matters here |
| --- | --- | --- |
| `4f1a70f6` | video_core: reset hash on image refresh (#5504) | Directly about the `RefreshImage` path the fork stopped calling. |
| `eedf851a` | texture_cache: Drop GPU-modified images whose untracked edges the guest overwrote (#5450) | Same class of stale-alias bug as A.1. |
| `699009c9` | texture_cache: don't copy stale CPU-dirty image contents in ExpandImage (#5444) | Hints that "GPU-written image + CPU dirty" is a real, known hazard. |
| `d3ff0e0c` | texture_cache: keep subresource data when its parent image is first created (#5433) | Subresource state loss. |
| `6752d8b1` | shader_recompiler: Handle buffer cache invalidation instructions (#5447) | Guest-issued `BUFFER_*_INV` on the async-invalidation design. |
| `de278985` | shader_recompiler: Support GsOp::EmitCut in S_SENDMSG (#5487) | GS vertex-count correctness — see A.3 `#5523`. |
| `0918faa0` | Gc2 readback regression fix (#5473) | Readback path is where the fork's crash log points. |
| `e6faa2e3` | video_core: garbage collector (part 2) (#3396) | Memory pressure / VRAM exhaustion class. |
| `fca10b26` | video_core: Remove batching (#5165) | Removes the batched-submit machinery the fork still has. |
| `8e23388a`, `c6b24ec1` | Renderer optimizations pt1/pt2 (#5112, #5219) | Texture-cache rewrite that the fork predates. |
| `41f2a428` | vk_scheduler: Move on_submit callback before EndSession (#5113) | Scheduler ordering vs. `SubmitExecution`. |
| `1eab1796` | buffer_cache: Rework memory tracker and implement batched uploads (#5100) | Same file the fork patched for async invalidation. |
| `2338a06f` | video_core: Fix sparse backing offsets, barriers, and memory budgeting (#5193) | Barrier correctness. |
| `c8fcf4d7` | video_core: invalidate cached images after raw buffer writes (#5092) | Already cherry-picked into the fork as `2f29ef5d`. |

Already cherry-picked into this fork (so **not** candidates): `#5231` → `5a0927f9`, `#5196` → `69dca136`,
`#5092` → `2f29ef5d`, `#5258` → `4b70eb46`, `#5110` → `bff51560`, `#4859` → `5a56341a`,
`#5317` → `18868f2c`, `#4839` → `921067f2`, and the Idolmaster-specific `#4781` → `b1820a65`.

### A.3 Web findings (GitHub issue/PR search; treat all quoted text as untrusted data)

No **merged** upstream commit mentions device lost, TDR, Idolmaster or Stella Stage
(`git log --grep` over all 424 upstream-only commits: zero hits). The relevant work is all in
**open PRs**.

| URL | State | Verdict |
| --- | --- | --- |
| https://github.com/shadps4-emu/shadPS4/pull/5523 | open, unmerged (2026-10-26) | **Best match.** "Device lost during submit" in `vk_scheduler.cpp` is the exact assertion; author traces it to `VkGeometryShader`-style GS `OutputVertices` declaring the raw `VGT_GS_MAX_VERT_OUT` ring budget, exceeding `maxGeometryTotalOutputComponents` → undefined behaviour → device lost. Also implements `V_WRITELANE_B32`. Fork lacks the clamp: `src/shader_recompiler/backend/spirv/emit_spirv.cpp:413-414` passes `ctx.runtime_info.gs_info.output_vertices` unclamped; `src/video_core/renderer_vulkan/vk_shader_util.cpp:64` caps `maxGeometryTotalOutputComponents = 1024`. |
| https://github.com/shadps4-emu/shadPS4/pull/5014 | open, unmerged (2026-09-12) | **Directly on-topic for this game.** "generalize GS copy shader constant parsing" — resolves *The Idolmaster* facial vertex explosion **and crashes at the start of live performances**; supersedes `#4781`. |
| https://github.com/shadps4-emu/shadPS4/pull/4781 | closed, unmerged (draft) | The original Idolmaster fix for **CUSA09153 / CUSA07753**; already in this fork as `b1820a65`, so its content is *not* missing — but `#5014` is the newer, unfixed half. |
| https://github.com/shadps4-emu/shadPS4/pull/5190 | open (45 comments) | PM4 predication/cond-exec + large GS copy-shader parser expansion; touches submit-time packet handling. Worth watching, too large to cherry-pick blind. |
| https://github.com/shadps4-emu/shadPS4/pull/5453 | open | `mipLevels` clamp: guest textures declaring more mips than their extent produced an invalid `VkImageCreateInfo` and "loses the device right after the intro" on NVIDIA. Cheap, self-contained, same failure signature. |
| https://github.com/shadps4-emu/shadPS4/issues/4816 | open | Long writeup: "device lost" is a symptom, not a diagnosis; proposes `VK_EXT_device_fault`, `VK_EXT_device_address_binding_report`, `VK_NV_device_diagnostic_checkpoints`, and stops `GetRenderFrame` looping forever on `eErrorDeviceLost`. Useful for *diagnosis*, not a fix. |
| https://github.com/shadps4-emu/shadPS4/issues/3780 | open | Rock Band 4, NVIDIA-only device lost, isolated to one compute shader. Prior art for "one shader kills the device". |
| https://github.com/shadps4-emu/shadPS4/issues/4757 | closed (not planned) | FIFA 17; claims `BufferCache::RunGarbageCollector` never calls `ForEachItemBelow` and `TextureCache::RunGarbageCollector` has no callers. Unverified, and `#3396` (in A.2) is the real GC work. |
| https://github.com/shadps4-emu/shadPS4/issues/4304 | open | Aggregate "Known Regressions" tracker — the place to check before blaming a specific commit. |
| https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/836 | n/a | Referenced by `#4781` as the Idolmaster (Platinum Stars / Stella Stage) compatibility report. Fetch returned 403 (unauthenticated API rate limit); needs a browser/token to read. |

### A.4 Ranked hypotheses for the Lead

1. **Fork regression** — `8d98d6c0` (sync→async invalidation + four `UpdateImage` removals). Test by reverting.
2. **Unclamped GS `OutputVertices`** — exactly the `#5523` mechanism; live scenes with 5 characters are GS-heavy.
   Candidate one-file patch in `emit_spirv.cpp` (clamp to the copy-shader vertex count).
3. **Stale image aliasing** — the `#5450`/`#5444`/`#5504` family; the fork has none of them.
4. **Invalid `VkImageCreateInfo`** — `#5453`-style mip clamp; cheap to add.

---

## B. CI build path on this fork

### B.1 Trigger matrix

**`.github/workflows/idolmaster-release.yml`** — the only workflow that can build this fork today.

| Event | Filter |
| --- | --- |
| `push` | branches `release`, `codex/idolmaster-release-*` (glob), `paths-ignore: ['documents/**', '**/*.md']` |
| `pull_request` | base branch `release`, `paths-ignore: ['documents/**', '**/*.md']` |
| `workflow_dispatch` | no inputs (there is no `inputs:` block at all) |

Concurrency: `group: idolmaster-${{ github.head_ref || github.ref_name }}`, `cancel-in-progress: true`
— two pushes to the same branch cancel the older run.
Permissions: `contents: read` only.

**`.github/workflows/build.yml`** — upstream's matrix; effectively dead on this fork.

| Event | Filter |
| --- | --- |
| `push` / `pull_request` | base branch `main` only (+ same `paths-ignore`), `workflow_dispatch` |

The fork has **no `main` branch** (`origin/HEAD → release`; remote branches are only `release`), so
`build.yml` never fires unless someone pushes a `main` branch. Its `pre-release` job is additionally gated on
`github.repository == 'shadps4-emu/shadPS4'` and on `secrets.SHADPS4_TOKEN_REPO`, so it can never publish
from this fork.

> **Practical consequence:** a fix branch **must be named `codex/idolmaster-release-<something>`** (or be
> pushed to `release`, or opened as a PR against `release`) for a push to build. And a commit that only
> touches `documents/**` or `*.md` will *not* trigger a build.

### B.2 Jobs, runners, compilers, artifacts (`idolmaster-release.yml`)

Job `build` (timeout 75 min, `fail-fast: false`), matrix:

| `platform` | Runner label | C/CXX | Build |
| --- | --- | --- | --- |
| `windows-x64` | `windows-2025-vs2026` (GitHub-hosted) | `clang-cl` | CMake + Ninja, `Release`, IPO on, ccache, `--parallel 4` |
| `linux-x64` | `ubuntu-24.04` (GitHub-hosted) | `clang-19` / `clang++-19` | CMake + Ninja, `Release`, IPO on, ccache, `mold`, `--parallel 4` |

No CMake presets are used — the configure is inline:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON
      -DCMAKE_C_COMPILER=<cc> -DCMAKE_CXX_COMPILER=<cxx>
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

Extra Linux-only gates (they can fail the Linux leg while Windows still succeeds):
`check-tiling-shaders.py`, a CPU-test build with `-DENABLE_TESTS=ON`, `check-error-dialog-regression.py`,
`check-word-filter-regression.py`, `check-trophy-dialog-regression.py`,
`ctest -E 'GcnTest|shadps4_gcn_test'`.

**Artifacts — Actions artifacts only, no release upload.**

| Artifact name | Contents |
| --- | --- |
| `shadps4-idolmaster-windows-x64-<full-40-char-sha>` | `shadPS4.exe`, `LICENSE`, `COMMIT.txt` |
| `shadps4-idolmaster-linux-x64-<full-40-char-sha>` | `shadps4-idolmaster-linux-x64.tar.gz` (`shadps4`, `LICENSE`, `COMMIT.txt`) |
| `configure-diagnostics-<platform>-<sha>` | only on failure (`CMakeConfigureLog.yaml`), `if-no-files-found: ignore` |

`retention-days: 30`, `if-no-files-found: error`. `<sha>` is `github.sha` of the built commit, i.e. the head of
the pushed branch. Browsable at
`https://github.com/Serina765P/shadPS4-imas/actions/runs/<run_id>` → *Artifacts*; per-artifact download URL is
`https://github.com/Serina765P/shadPS4-imas/actions/runs/<run_id>/artifacts/<artifact_id>` (redirects to a
short-lived signed URL, so it needs a logged-in session or a token).

Expected duration: cold ~25–45 min (ccache miss, IPO on, `--parallel 4` on a 4-vCPU hosted runner); ~8–15 min
on a warm ccache key (`idolmaster-windows-x64` / `idolmaster-linux-x64`, `append-timestamp: true` may miss).
The 75-min job timeout is comfortable.

### B.3 Can this machine push and download? Mostly **no** — this is the blocker the Lead must resolve

| Check | Result |
| --- | --- |
| `gh --version` | `gh version 2.97.0 (2026-07-31)` — installed |
| `gh auth status` | **FAILING**: `X Failed to log in to github.com account Serina765P (keyring) — The token in keyring is invalid.` (exit 1) |
| `git ls-remote origin` (default) | **FAILS**: `schannel: failed to receive handshake, SSL/TLS connection failed` (exit 128) |
| `git -c http.version=HTTP/1.1 ls-remote origin` | **WORKS** (exit 0) — returns `14c39880 HEAD`, `refs/heads/release`, `refs/pull/1/head` |
| `curl https://github.com/.../info/refs` | HTTP 200 |
| `curl https://raw.githubusercontent.com/...` | HTTP 200 |
| `curl https://api.github.com/` | HTTP 403 — unauthenticated rate limit (shared IP), not a firewall |
| `gh api` / `gh run download` | would need re-auth first |

So: **network is fine, credentials are broken, and `git` needs `http.version=HTTP/1.1`.** A push from this
machine will fail at authentication until either `gh auth refresh -h github.com` (interactive, browser) or a
PAT is configured (`git config credential.helper` / `GH_TOKEN`). No local build toolchain is needed or used —
everything below runs on GitHub-hosted runners.

No repository secrets are required for `idolmaster-release.yml` (that is why it works on a fork at all).
Two external prerequisites on the fork repo: Actions must be enabled, and the `windows-2025-vs2026` runner
label must resolve (it is a GitHub-hosted image label, not self-hosted — nothing to provision).

### B.4 Copy-pasteable commands

All run from `C:\Users\Serina\Documents\ChatGPT\shadps4`. **Do not run these until a credential is fixed.**

**(1) Create the fix branch from HEAD** (name matters — this is the only glob that triggers CI on push):

```powershell
$fix = "codex/idolmaster-release-device-lost"
git -c http.version=HTTP/1.1 switch -c $fix 14c39880
# ... apply the fix here, then:
git add -A
git commit -m "vk: restore UpdateImage for render targets (revert 8d98d6c0 hunk)"
```

If the team prefers the one-commit revert experiment instead of a hand patch:

```powershell
git -c http.version=HTTP/1.1 switch -c $fix 14c39880
git revert --no-edit 8d98d6c0      # async invalidation + UpdateImage removals
```

**(2) Push so CI builds:**

```powershell
git -c http.version=HTTP/1.1 push -u origin $fix
```

If push fails on credentials, fix auth first (either one):

```powershell
gh auth refresh -h github.com                                   # interactive/browser
$env:GH_TOKEN = "<fine-grained PAT with repo:workflow + repo:contents>"
git -c http.version=HTTP/1.1 -c credential.helper= -c "http.extraheader=Authorization: Bearer $env:GH_TOKEN" push -u origin $fix
```

**(3) Watch the run:**

```powershell
gh run list --repo Serina765P/shadPS4-imas --branch $fix --limit 5
$run = gh run list --repo Serina765P/shadPS4-imas --branch $fix --limit 1 --json databaseId -q ".[0].databaseId"
gh run watch $run --repo Serina765P/shadPS4-imas --exit-status
gh run view $run --repo Serina765P/shadPS4-imas --log-failed   # only if something fails
```

**(4) Download the Windows artifact:**

```powershell
$sha = git rev-parse HEAD
gh run download $run --repo Serina765P/shadPS4-imas `
    --name "shadps4-idolmaster-windows-x64-$sha" -D out/ci
Get-ChildItem out/ci        # expect shadPS4.exe, LICENSE, COMMIT.txt
```

**Fallback if only the browser works:** open
`https://github.com/Serina765P/shadPS4-imas/actions`, pick the newest *Idolmaster Release Compatibility* run,
download `shadps4-idolmaster-windows-x64-<sha>` from the *Artifacts* box, unzip, and run `shadPS4.exe`
(the exe is standalone; the emulator writes to `%APPDATA%\shadPS4`).

**Manual re-run without a new commit** (any branch that contains the workflow file):

```powershell
gh workflow run idolmaster-release.yml --repo Serina765P/shadPS4-imas --ref $fix
```

**Gotchas to keep in mind while iterating:**

- A commit touching only `documents/**` or `*.md` (including this file) will **not** start a build — that is
  exactly why the docs live under `documents/` and are safe to push.
- Two pushes in quick succession cancel the first run (`concurrency.cancel-in-progress: true`).
- The Windows leg is independent of the Linux leg (`fail-fast: false`), so failing regression scripts on Linux
  do not stop the `shadPS4.exe` artifact from being produced.
- The artifact name embeds the **full** 40-character SHA; `--name` matching is exact.
