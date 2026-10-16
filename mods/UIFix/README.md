# UI Fix

A Forge PC mod for the recompiled PC build of Monster Hunter Generations Ultimate (1.4.0). It does
two things, in one place:

1. **Undoes the stretching** of the HUD and menus on screens wider than 16:9 (21:9, 32:9...). The
   game draws its UI for 16:9 and stretches it over the whole window; UI Fix squeezes it back, so
   the clock is round again and the health bar has its normal shape.
2. **Scales and moves single parts of the UI** (health bar, map, item bar, menus...) by dragging
   them on screen, plus one global size for everything.

It replaces the earlier **HUD Fix** and **UI Scale** mods, which did the two halves separately.

Needs **Forge PC 0.3** or newer.

## Install

Copy the `UIFix` folder next to the `Forge` folder, and remove `HudFix` and `UIScale` if you had
them (the menu warns in red when they are still there):

```
<game>/mods/0100770008DD8000/Forge/...                                 Forge PC
<game>/mods/0100770008DD8000/UIFix/plugins/uifix.dll                    (Linux: uifix.so)
<game>/mods/0100770008DD8000/UIFix/plugins/presets/*.ini                the defaults
```

On first start UI Fix saves its default as `UIFix/plugins/uifix.ini`. It is the same for every
screen: the UI squeezed back to 16:9 where the screen is wider, shrunk to
75 %, with the parts placed where they sit well (made on a 21:9 screen).

If HudFix / UIScale settings are found next to it, those are taken over instead.

Uninstall: delete the `UIFix` folder.

## In game

Press **Insert** for the Forge menu, then open **Plugin UI > UI Fix** (the section is collapsed by
default; the element boxes only show while it is open):

- **UI Fix on**: switches everything off to get the game's own UI back.
- **Undo the stretching on wide screens**: the destretch alone.
- **Global scale**: the size of the whole UI (1 = the game's own size). Full-screen pictures and
  backgrounds are never scaled.
- **Edit the UI**: shows a box around every part of the UI.
  - **Click** a part to select it (yellow box), then set its **Scale**. A part's own scale replaces
    the global one.
  - **Drag** the yellow box to move it. Its edges snap onto the screen edges and onto the edges of
    other parts; hold **Alt** to move without snapping.
  - **Every layout like it**: one setting for all screens of the same kind (for example every
    area map) instead of just this one.
  - **Reset element** puts that part back.
- **Default** / **Original game UI**: start over with the default, or with no changes
  at all (both ask first).
- **Advanced**: a **Depth** slider for the boxes (1 = whole parts, higher = their pieces), the list
  of parts on screen, and a **Debug** section for modders (freeze, piece boxes, labels, full-screen
  pictures, saving a list of the UI, the destretch table of every screen).

Everything is saved to `uifix.ini` as you go. Edit it by hand only with the game closed (the menu
rewrites it).

## Notes

- The minimap (also the one without a map item) is squeezed, scaled and moved like any other part.
  While a loading screen is up the game's own map movement is left alone (the map is only squeezed
  about itself) and your changes apply again afterwards.
- The item-set menus shared by the item pouch, item box, supply box and delivery box shrink about the
  same point as the screen showing them. Screens you open again keep their scale from the first frame.
- Anything whose box covers the whole 16:9 screen (title and loading pictures, backgrounds, fades)
  stays stretched over the window on purpose; it is not squeezed, scaled or moved, because
  narrowing it gives black bars. Parts of a picture built from smaller pieces are still squeezed.
- The minimap is exempt from your changes only while a loading screen is up; it takes them from its
  first frame after loading.
- Some HUD parts with scrolling icons (item bar, ammo) can cut their icons off when scaled a lot.
- Moving a part that sits inside another resized part moves it a little less than the mouse;
  snapping still lines it up.
- Screens narrower than 16:9 are left as the game draws them.

## License

MIT (`LICENSE.txt`). Dear ImGui is compiled in (MIT, `LICENSE-imgui.txt`); the Forge PC plugin API
comes from MHGU Forge by Fexty (MIT, `LICENSE-MHGU-Forge.txt`). The `.gui` layout format
knowledge is based on Handburger's MHGU-Modding documentation (reference only).
