# Working in this repository

- Do all work in the primary checkout on `main`. Do not create or use worktrees or other branches.
- Before editing, inspect `git status` and run `scripts/sync-source.ps1`. It fetches `origin/main` and fast-forwards a clean checkout when needed. Preserve local edits and commits; if synchronization is blocked, reconcile them with the latest remote history before continuing. Never reset, force-push, or discard work to synchronize.
- Before a release, commit the reviewed changes on `main`, push normally, and run `scripts/sync-source.ps1 -CheckOnly`. Local `HEAD` and `origin/main` must match. `release.ps1 -Publish` enforces this both before preparation and before publication.
- After publishing, fetch and verify local `main`, `origin/main`, and the release tag identify the same source commit. Verify the live product site's portable and installer links target that release and their downloads match the signed release assets.
- Production `vmd.exe` must exclude test switches and instrumentation. Keep them behind `VMD_TESTING` in `vmd-test.exe`; never package the test executable.
- Preserve fast startup, document loading, first-match display, and navigation. Keep UI changes narrow and run the relevant performance and behavior checks.
