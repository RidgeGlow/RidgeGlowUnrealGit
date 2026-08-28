# Licensing notice

This plugin is distributed under two licenses. Which one applies depends on the file.

## Summary

| If you… | You are governed by |
|---|---|
| Bought the plugin on Fab | Epic's Fab Standard License (Personal or Professional tier) |
| Cloned, forked or submoduled this repository | The two licenses below, per file |

The plugin's module is declared `"Type": "UncookedOnly"`, which means it is an **editor-only**
module. It is not compiled into, and never ships inside, a packaged game. Nothing in either
license reaches a product you build with it.

## Upstream code — MIT ([`LICENSE-MIT.txt`](LICENSE-MIT.txt))

This project is a fork of [ProjectBorealis/UEGitPlugin](https://github.com/ProjectBorealis/UEGitPlugin),
itself a refactor of [SRombauts/UE4GitPlugin](https://github.com/SRombauts/UE4GitPlugin).

Everything under `Source/GitSourceControl/` that predates this fork remains under the MIT
license, with the original copyright notices intact:

- Copyright (c) 2021-2023 Project Borealis
- Copyright (c) 2014-2020 Sebastien Rombauts

MIT rights in that code cannot be, and are not being, withdrawn.

## RidgeGlow contributions — FSL-1.1-MIT ([`LICENSE`](LICENSE))

New work authored by RidgeGlow is licensed under the Functional Source License 1.1 with an
MIT future grant. This covers:

- `Source/GitSourceControl/Private/GitLfsLib.h` / `.cpp`
- `Source/ThirdParty/GitLfsLib/`
- `.github/workflows/`
- The `libgitlfs/` package in [RidgeGlow/git-lfs-lib](https://github.com/RidgeGlow/git-lfs-lib)

**In plain terms:** you may use, modify and redistribute it for any purpose *except* a
Competing Use — making it available to others in a commercial product or service that
substitutes for, or offers substantially similar functionality to, this plugin. Internal use
by a company, including commercial game development, is explicitly permitted. Each version
converts to MIT two years after its release.

## Third-party components

### Git LFS

The plugin loads a shared library built from [RidgeGlow/git-lfs-lib](https://github.com/RidgeGlow/git-lfs-lib),
a fork of [git-lfs/git-lfs](https://github.com/git-lfs/git-lfs).

Git LFS is licensed under the MIT license, Copyright (c) GitHub, Inc. and Git LFS
contributors. Its full license text and the licenses of its Go dependency tree are
reproduced in the `licenses/` directory of the released library artifacts.

Git LFS itself is not modified by the fork; the fork adds only the additive `libgitlfs/`
package and its build workflow.
