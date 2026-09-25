# Dependency patches

These patches apply to the pinned dependency revisions in `lib/`, including
their nested submodules. Apply each patch from its dependency directory with
`git apply --unidiff-zero <patch>`.

| Patch | Dependency directory |
| --- | --- |
| hlslpp-scalar-alignment.patch | lib/rt64/src/contrib/hlslpp |
| re-spirv-floating-predicates.patch | lib/rt64/src/contrib/re-spirv |
| n64recomp-jfg-overlays.patch | lib/N64ModernRuntime/N64Recomp |
| n64modernruntime-jfc-runtime.patch | lib/N64ModernRuntime |
| recompfrontend-jfc-runtime.patch | lib/RecompFrontend |
| plume-jfc-frame-capture.patch | lib/rt64/src/contrib/plume |
| rt64-jfc-runtime.patch | lib/rt64 |
