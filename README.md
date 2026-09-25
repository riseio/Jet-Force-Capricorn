# Jet Force Capricorn

A native PC and Steam Deck port of **Jet Force Gemini**, with widescreen support,
smooth interpolated rendering, modern controls and loads of optional quality-of-life improvements.

## Download and play

Get the latest Windows portable EXE or Linux AppImage from
[Releases](https://github.com/riseio/Jet-Force-Capricorn/releases).
You need your own **Jet Force Gemini (USA), revision 0** ROM in `.z64` format. Select it when prompted on first launch.

- **Windows:** Run `JetForceCapricorn-Portable.exe` from a writable folder.
  Saves, settings and caches stay in the adjacent `data` folder. Keep that folder
  when updating or moving the game. The EXE includes its runtime libraries.
- **Steam Deck:** Make the AppImage executable, add it to Steam as a non-Steam
  game in Desktop Mode and use a Gamepad layout. Launch it from Gaming Mode
  without Proton.
- **Linux:** Make the AppImage executable and open it. It includes the C/C++
  runtime and application libraries; your system supplies the graphics drivers
  and desktop services. Saves and settings use `~/.config/JetForceCapricorn`.

Enhanced textures are prepared locally from your ROM and cached for later
launches. Switch them in **Graphics > Enhanced Textures**, or press View / Select
on a controller to swap them in at anytime during play.

## Enhancements

- Widescreen and ultrawide support, with adapted menus and HUD placement.
- A complete control overhaul. The original controls are completely replaced with modern twinstick.
- Higher resolutions, anti-aliasing and interpolated rendering, with display-refresh
  and manual frame-rate settings.
- Mouse aiming and dual-stick controls: move and strafe with the left stick,
  look and aim with the right stick. You can also now manual aim while crouched.
- Remappable controls, mouse and stick sensitivity, deadzone, inversion and rumble settings.
- Sniper aiming sensitivity scales with zoom.
- Adjustable draw distance and fog strength.
- Persistent save states alongside the original game saves.
- Direct weapon shortcuts, quick throwable grenades.

## Quality of life

Open settings with **Escape / R4**, or choose Options in the pause menu, then
select **QOL**. Each setting is independent. Defaults below apply to a fresh
configuration; existing preferences are kept.

| Setting | Default | What it does |
| --- | --- | --- |
| Modern Health Display | Off | Shows the current health container as metroid style capsule healthbar above the weapon panel. |
| Room Collectible Counters | On | Shows your Mizar token total and Tribals still missing in the current room. Lost Tribals remain counted as missing. |
| Area Collectible Tracker | On | Shows remaining keys, including coloured keys, spaceship parts and Floyd robot components, including chest contents and event rewards. Icons stack above tokens and Tribals; collected categories disappear. |
| Weapon Shortcut HUD | On | Shows the four assigned weapon icons in a top-right D-pad, with numbers 1-4 for keyboard shortcuts. |
| Cutscene Skipping | On | Press Start / Enter to skip a cutscene. |
| Invulnerable Tribals | Off | Prevents Tribals from being killed. |
| Unlimited Lives | On | Restarts after death as though an extra life were available, without consuming continues. |
| Persistent Tribal Rescues | Off | Keeps individually rescued Tribals rescued when revisiting levels in the same save slot. |
| Final Tribal Requirement | Original | Choose the original rescued-Tribal requirement or reduce the final-access threshold to 70%, 50% or 25%. True 100% completion remains separate so you can still go for that if you really want. |
| Skip Seen Ship Landings | Off | Automatically skips a ship landing after you have watched that landing to completion in the same save slot. |
| Quick Character Switching | Off | Switches to the next unlocked character at that character's saved location without returning to space. |
| Skip Seen Character Travel | Off | Skips repeat character-selection travel sequences. |
| Sprint Mode | Off | Choose Hold or Toggle for up to 1.5x grounded movement speed. |
| Double Pistol Recharge | Off | Recharges the default pistol twice as fast, with its normal firing rate. |
| Double Ammo Pickups | Off | Doubles small, large, enemy-dropped and duplicate-weapon ammo awards, capped at normal capacity. |
| Persistent Dropped Pickups | Off | Keeps dropped health, ammo and collectible heads until collected or the map is left. |
| Mission Complete Refill | On | Refills health and owned weapon ammo when you dismiss the mission-complete screen. |
| Double Enemy Drop Chance | On | Raises random enemy weapon/ammo drop chance from one in three to two in three. Guaranteed rewards and pickup quantities stay the same. |
| Fast Throws | On | Halves throwables animation, release and recovery times. This makes throwing grenades feel way better and more useful. |
| Halo-style Health | Off | After 30 damage-free gameplay seconds, restores missing health every five seconds over the next 60 seconds. |
| Ship Part Scanner | Off | Choose Audio, Visual or Audio + Visual guidance towards missing ship parts. Visual hints also need Show Guidance Text. |
| Show Guidance Text | Off | Shows bottom-left ship-part scanner hints and final Tribal-requirement guidance. I use this for debugging so you likely won't need it. |

### Weapon shortcuts and throwables

Hold **Ctrl / L4** and press a weapon slot to assign the equipped weapon.
Press the slot alone to equip it directly. Slots 1-4 correspond to D-pad
Up, Right, Down and Left. Assignments are saved separately for each character.

Press **G / Y** to use the assigned throwable and return to your weapon. Grenades
are the default. To assign another, equip it, hold **Ctrl / L4**, then press
**G / Y**. Grenades, shurikens, fish food, plasma bombs, remote mines and cluster
bombs can be assigned; flares keep their normal weapon controls. The bottom-left
icon and count show the assigned item and remaining ammo.
Because Flares have their arm and throw stages separate I didn't include them... soz

### Saving

Press **F6 / L5** during gameplay to save a state, then **F9 / R5** to restore it,
including after restarting the game. Saving again replaces the manual state.
Ordinary game saves are separate. Restoring a state keeps your current QOL preferences.

## PC controls

| Input | Action |
| --- | --- |
| WASD / mouse | Move and strafe / look and aim |
| Left mouse button | Fire |
| Right mouse button | Hold for fine aim / sniper scope |
| W / S while scoped | Adjust sniper zoom |
| Space | Jump / stand |
| C | Crouch / crawl |
| F | Interact / advance dialogue |
| Shift | Sprint, when enabled |
| E / R or wheel down / up | Next / previous weapon |
| 1 / 2 / 3 / 4 | Select weapon slot 1 / 2 / 3 / 4 |
| Ctrl + 1 / 2 / 3 / 4 | Assign equipped weapon to that slot |
| G | Use assigned throwable |
| Ctrl + G | Assign equipped throwable |
| Ctrl + T | Change unlocked character, when enabled |
| Enter | Pause / resume; skip cutscene when enabled; confirm in menus |
| Escape | Open / close settings |
| F6 / F9 | Save / restore state |
| F11 or Alt + Enter | Toggle fullscreen |

For textures, open **Escape > Graphics > Enhanced Textures**.
Change sensitivity and inversion in Controls, or remap actions in Bindings.

## Steam Deck controls

Use Steam's **Gamepad** layout. These are the default in-game bindings:

| Input | Action |
| --- | --- |
| Left stick | Move / strafe; forward / back adjusts zoom while scoped |
| Right stick | Look / aim |
| R2 | Fire |
| L2 | Hold for fine aim / sniper scope |
| A | Jump / stand; confirm in menus |
| B | Crouch / crawl; back in menus |
| X | Interact / advance dialogue; sprint when enabled |
| Y | Use assigned throwable |
| L1 / R1 | Next / previous weapon; change settings tabs |
| D-pad Up / Right / Down / Left | Select weapon slot 1 / 2 / 3 / 4; navigate menus |
| L4 + D-pad direction | Assign equipped weapon to that slot |
| L4 + Y | Assign equipped throwable |
| L4 + B, or L3 + R3 together | Change unlocked character, when enabled |
| Menu / Start | Pause / resume; skip cutscene when enabled; apply settings |
| View / Select | Switch Original / Enhanced textures |
| L4 | Hold as the shortcut modifier |
| R4 | Open / close settings |
| L5 / R5 | Save / restore state |

## Disclaimer

Jet Force Capricorn is an unofficial fan project not associated with or containing the original game in any way.
It is not affiliated with, endorsed by, or sponsored by Nintendo, Rare, Microsoft, or any other
rights holder.

**We do not provide the original game.** This repository and its downloads do
not include a Jet Force Gemini ROM, music, sounds, textures, models, levels, or other
extracted original game assets. We do not provide ROM downloads or links to them.
Game assets are loaded or generated locally from the ROM you supply.

**You must own a legally purchased copy of Jet Force Gemini and provide your own
lawfully obtained ROM dump** of the supported US revision-0 release to use this
project. The port is not a substitute for owning the original game. Do not
upload or share ROMs or extracted game assets in this repository or its issues.

Jet Force Gemini, its characters, names, artwork, music, trademarks and all other
original game content belong to their respective copyright and trademark
holders. We claim no ownership of that content. This project grants no rights
to copy or redistribute the original game or its assets.

## Project

Built with N64Recomp, N64ModernRuntime, RecompFrontend and RT64.

[Report a bug](https://github.com/riseio/Jet-Force-Capricorn/issues) When raising issues please make sure you include the build version you experienced the bug in ^_^
