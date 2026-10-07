#!/usr/bin/env bash
# Builds Forge PC for Linux (forge.so) plus the HUD Fix plugin, and lays out an install folder:
# dist-linux/mods/0100770008DD8000/  -> copy "mods" into the game folder.
# Needs g++ (C++20) and the Vulkan headers (libvulkan-dev, or VULKAN_SDK=<sdk dir>); only the
# headers are used, Vulkan functions come from the game.
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-g++}
CC=${CC:-gcc}
VK_INC=""
if [ -n "${VULKAN_SDK:-}" ]; then
  for d in "$VULKAN_SDK/include" "$VULKAN_SDK/Include"; do
    [ -f "$d/vulkan/vulkan.h" ] && VK_INC="-I$d" && break
  done
fi
if [ -z "$VK_INC" ] && [ ! -f /usr/include/vulkan/vulkan.h ]; then
  echo "[error] Vulkan headers not found: install libvulkan-dev (or set VULKAN_SDK)." >&2
  exit 1
fi

OUT=dist-linux/mods/0100770008DD8000
OBJ=build-linux/obj
mkdir -p build-linux "$OBJ/forge" "$OBJ/hudfix" "$OUT/Forge/fonts" "$OUT/Forge/licenses" \
  "$OUT/HudFix/plugins" build-linux/examples

# Hidden symbols + -Bsymbolic: every .so keeps its own Dear ImGui and C++ runtime bits, only the
# entry points (modhost_attach / forge_*) are visible. Static libstdc++: no dependency on the
# player's version.
COMMON="-O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden"
LDFLAGS="-shared -Wl,-Bsymbolic -Wl,--no-undefined -Wl,--exclude-libs,ALL -static-libstdc++ -static-libgcc"
# export lists: the loader exports modhost_attach, plugins their forge_* entry points
printf '{ global: modhost_attach; local: *; };
' > build-linux/forge.map
printf '{ global: forge_*; local: *; };
' > build-linux/plugin.map
IMGUI="third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp"

build_objs() { # <objdir> <flags> <sources...>
  local dir=$1 flags=$2; shift 2
  local pids=() s
  for s in "$@"; do
    "$CXX" $flags -c "$s" -o "$dir/$(basename "${s%.*}").o" &
    pids+=($!)
  done
  local ok=0 p
  for p in "${pids[@]}"; do wait "$p" || ok=1; done
  return $ok
}

FORGE_FLAGS="$COMMON -std=c++20 -Wall -DVK_NO_PROTOTYPES -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES \
  -Iinclude -Ithird_party/imgui $VK_INC"
build_objs "$OBJ/forge" "$FORGE_FLAGS" src/loader.cpp src/overlay.cpp $IMGUI \
  third_party/imgui/imgui_demo.cpp third_party/imgui/backends/imgui_impl_vulkan.cpp
"$CXX" $LDFLAGS -Wl,--version-script=build-linux/forge.map -o "$OUT/Forge/forge.so" "$OBJ"/forge/*.o -ldl -lpthread

cp -f data/Roboto-Medium.ttf "$OUT/Forge/fonts/"
[ -f "$OUT/Forge/forge.ini" ] || cp data/forge.ini "$OUT/Forge/"
cp -f LICENSE "$OUT/Forge/licenses/LICENSE.txt"
cp -f LICENSE-MHGU-Forge.txt THIRD_PARTY_NOTICES.md data/LICENSE-Roboto.txt "$OUT/Forge/licenses/"
cp -f third_party/imgui/LICENSE.txt "$OUT/Forge/licenses/LICENSE-imgui.txt"

# example plugin: compiled to check it still builds, not installed
"$CC" -O2 -fPIC -fvisibility=hidden -Wall -Iinclude -shared -Wl,--no-undefined \
  examples/hello/hello.c -o build-linux/examples/hello.so

# HUD Fix mod (plugin with its own Dear ImGui, same version as forge.so)
build_objs "$OBJ/hudfix" "$COMMON -std=c++20 -Wall -Iinclude -Ithird_party/imgui" \
  mods/HudFix/hudfix.cpp $IMGUI
"$CXX" $LDFLAGS -Wl,--version-script=build-linux/plugin.map   -o "$OUT/HudFix/plugins/hudfix.so" "$OBJ"/hudfix/*.o -ldl
cp -f LICENSE "$OUT/HudFix/LICENSE.txt"
cp -f LICENSE-MHGU-Forge.txt "$OUT/HudFix/"
cp -f third_party/imgui/LICENSE.txt "$OUT/HudFix/LICENSE-imgui.txt"
[ -f "$OUT/HudFix/plugins/hudfix_layouts.ini" ] || cp mods/HudFix/hudfix_layouts.ini "$OUT/HudFix/plugins/"

echo
echo "Built: $OUT/Forge/forge.so  and  $OUT/HudFix/plugins/hudfix.so"
