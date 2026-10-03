# Third-party notices

Forge PC (the loader `forge.dll`, the plugin SDK and the HUD Fix plugin) includes or ships
the following third-party work.

## MHGU Forge

- What: the design, plugin API, menu and style this project ports to the PC build.
- Author: Fexty (https://github.com/Fexty12573/forge), with contributions by jeffi2287.
  Ported with the author's permission.
- License: MIT, Copyright (c) 2020 The Skyline Project, Copyright (c) 2026 Jeffi. Full text:
  `LICENSE-MHGU-Forge.txt` (shipped in `licenses/`).

## Dear ImGui

- What: the menu UI, compiled into `forge.dll` and `hudfix.dll` (`third_party/imgui`,
  v1.92.8, with the Vulkan backend).
- Author: Omar Cornut and contributors, https://github.com/ocornut/imgui
- License: MIT, Copyright (c) 2014-2026 Omar Cornut. Full text:
  `third_party/imgui/LICENSE.txt` (shipped as `licenses/LICENSE-imgui.txt`).

## Roboto font

- What: `data/Roboto-Medium.ttf` (shipped as `Forge/fonts/Roboto-Medium.ttf`), the menu font,
  as distributed with Dear ImGui and MHGU Forge.
- Author: Christian Robertson / Google, https://fonts.google.com/specimen/Roboto
- License: Apache License 2.0. Full text: `data/LICENSE-Roboto.txt` (shipped as
  `licenses/LICENSE-Roboto.txt`).

## References (no code included)

- MHGU-Modding by Handburger (https://github.com/RTHKKona/MHGU-Modding, MIT): file format
  documentation and 010 Editor templates. HUD Fix's understanding of the `.gui` layout format
  came from them.
- Skyline (https://github.com/skyline-dev/skyline): the base MHGU Forge is built on.
