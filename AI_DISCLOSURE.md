# AI disclosure and usage

Forge PC was built with the help of an AI coding assistant (a large language model), working under the direction of the maintainer. This page says what that means, so nobody has to guess. It applies to the loader, the Rust SDK and the example mod (UI Fix) in this repository.

## What the AI did
- Wrote and edited a large share of the source code: the loader, the in-game menu, hooks and overrides, the settings-file API, the plugin Vulkan present hook, call tracing, crash attribution, mod on/off, Reset scripts, the Rust SDK, and the build and package scripts.
- Wrote the example mod UI Fix (its hooks, layout handling and in-game editor) and the documentation, including this README.
- Did reverse-engineering legwork on the game: finding the functions and structures the hooks and UI Fix target, reading logs, and tracing calls from test runs.

## What the maintainer did
- Set the goals, decided what to build and what to drop, and chose every design direction (including the menu look and how mods are laid out).
- Ran every build and every in-game test on real hardware, and judged the results.
- Caught bugs by hand and found what was wrong from logs and screenshots, not just by building and testing.
- Reviewed what is published here and decided what goes public.

## How AI output is checked
- The loader and the Rust SDK run against a mock host in an automated test (loading, hooks, overrides, settings files, Reset scripts), then in the real game.
- Plugins are tested in game before a release: nothing is claimed to work because an AI wrote it. If it is not tested, it is not claimed.
- Release binaries are scanned for personal strings and build paths before they are published.

## What the AI did NOT do
- It did not use leaked or NDA material.
- This repository contains no game files, keys or game data. The mod interface is used with your own legally obtained copy.
- Forge PC is a port of Fexty's [MHGU Forge](https://github.com/Fexty12573/forge), done with the author's permission, and keeps its design, plugin API and licence notice. Third-party work is credited in `README.md` and `THIRD_PARTY_NOTICES.md`; reference-only material (such as the `.gui` layout documentation from MHGU-Modding) is not shipped as code.

## Licensing and responsibility
The code is released under the licence in this repository (MIT, see `LICENSE` and `LICENSE-MHGU-Forge.txt`). Everything published here is published as-is, whoever or whatever typed it, and the project is maintained under the GURecomp handle. Report problems as ordinary issues.

## Policy for contributions
Contributions that used AI tools are fine if you disclose it in the pull request, understand and have tested what you submit, and can answer questions about it. Do not submit code you cannot explain, and do not submit anything that includes leaked SDK material, keys or game data.

## Keeping this page current
When a new feature, mod or release is built with AI help, add it under "What the AI did", and add any new third-party work to the credits in `README.md` and `THIRD_PARTY_NOTICES.md` in the same commit.
