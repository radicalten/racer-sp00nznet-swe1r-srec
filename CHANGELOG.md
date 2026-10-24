# Changelog

## v0.1.0 (2026-10-24)

First release. Install with the recomp-launcher installer on the release page (or `Setup.cmd`): it
takes your own USA `.z64`, recompiles it on your machine and builds the game. Nothing from the game
is downloaded or shipped.

- Plays from boot through menus, tournament and free play into full races.
- RT64 renderer (DirectX 12): high resolution, MSAA, widescreen (experimental), texture packs, fullscreen (F11).
- Sound: the game's audio microcode recompiled, with a volume setting.
- Launcher and in-game menu (Esc or the controller's Back button): controls, graphics, sound, netplay and cheats.
- Controllers with drop-in/drop-out for 2 players, keyboard, rebinding.
- 2-player netplay over LAN or internet (deterministic lockstep).
- GameShark cheats from a Project64 `.cht` list (Settings > Cheats).
- Fixed: a mid-race freeze when the sound queue backed up.

Known issues: netplay is untested across two physical machines; widescreen leaves the game's 4:3
blur overlay; Windows only.
