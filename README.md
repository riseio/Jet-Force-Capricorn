# Jet Force Capricorn

A native PC and Steam Deck port with widescreen support,
interpolated rendering, modern controls and optional quality-of-life improvements.

## Download and play

Windows and Linux builds are available from [Releases](https://github.com/riseio/Jet-Force-Capricorn/releases).
You need your own **revision 0** ROM. The packages contain no ROM, extracted game assets or translated game code.

Both packages keep saves, settings and generated content in the adjacent `data`
folder. Keep that folder when replacing or moving the executable.

Enhanced textures are generated locally. Choose **Install enhanced
textures** in the launcher or Graphics settings, then switch them with
**Graphics > Enhanced Textures** or View / Select during play.

## Enhancements

- Widescreen and ultrawide support, with adjusted menus and HUD placement.
- Higher resolutions, anti-aliasing and interpolated rendering for high refresh rate displays.
- Mouse aiming and modern twin-stick controls, including new things like aiming while crouched.
- Adjustable field of view and camera distance.
- Separate horizontal and vertical inversion for each stick.
- Remappable controls, sensitivity, deadzones and rumble settings.
- Sniper aiming sensitivity that scales with zoom.
- Adjustable draw distance and fog strength.
- Cleaner original fonts and UI artwork, generated using images from your own supplied game.
- Direct weapon shortcuts, quick throwable selection and persistent save states.
- The game now saves at almost all area transitions not just the end of area ones.

## QOL Toggles

| Setting | Default | What it does |
| --- | --- | --- |
| Modern Health Display | Off | Shows reserve health containers and the current container as a capsule bar above the weapon panel. |
| Room Collectible Counters | On | Shows your Mizar token total and Tribals still missing in the current room. |
| Area Collectible Tracker | On | Shows remaining keys, including coloured keys, spaceship parts and Floyd robot components. |
| Weapon Shortcut HUD | On | Shows the four assigned weapon icons in a top-right D-pad, with numbers 1-4 for keyboard shortcuts. |
| Cutscene Skipping | On | Use Space, Enter, keypad Enter or Start to skip a cutscene. |
| Invulnerable Tribals | Off | Protects Tribals from lethal damage. |
| Unlimited Lives | On | Restarts after death as though an extra life were available, without consuming continues. |
| Persistent Tribal Rescues | On | Shares individually rescued Tribals across characters and return visits in the same save slot. |
| Final Tribal Requirement | Original | Choose the original requirement or reduce the final spaceship-part threshold to 70%, 50% or 25%.  |
| Skip Seen Ship Landings | Off | Automatically skips a ship landing after you have watched that landing to completion in the same save slot. |
| Quick Character Switching | Off | Switches to the next unlocked character at that character's saved location without returning to space. |
| Skip Seen Character Travel | Off | Skips repeat character-selection travel sequences. |
| Sprint Mode | Off | Choose Hold or Toggle for up to 1.5x grounded movement speed. |
| Double Pistol Recharge | Off | Recharges the default pistol twice as fast, with its normal firing rate. |
| Double Ammo Pickups | Off | Doubles small, large, enemy-dropped and duplicate-weapon ammo awards, capped at normal capacity. |
| Persistent Dropped Pickups | Off | Keeps dropped health, ammo and collectible heads until collected or the map is left. |
| Upgraded Shotgun | On | A button press fires six fully charged energy spheres with a wider spread, using one round. No need to hold anymore. |
| Upgraded Flamethrower | On | Half fuel use, 50% more burning damage and a hurt reaction whenever the enemy can be staggered. |
| Upgraded Shocker | On | Four times the beam range, arcs to up to three adjacent enemies and a reserve of at least 100. |
| Persistent Corpses | Off | Keeps bodies until the area is left. Turning this off resumes fading; memory cleanup can still remove bodies. |
| Mission Complete Refill | On | Refills health and owned weapon ammo when you dismiss the mission-complete screen. |
| Double Enemy Drop Chance | On | Raises random enemy weapon/ammo drop chance from one in three to two in three. |
| Fast Throws | On | Grenade and other throwable animations take half as long. |
| Double Robot Mission Time | Off | Doubles the time limit for a newly started or retried Floyd mission. |
| Hide Floyd Companion | Off | Hides the floyd robot and companion crosshair outside Floyd missions and story scenes. |
| Half Boss Health | Off | New boss fights and phases start at half health. Existing saved health is retained. |
| Boss Target Health | Off | Shows the health and vulnerability of the boss part under your aim. |
| Double Pistol Boss Damage | On | Pistol shots deal twice their normal damage to vulnerable boss parts. |
| Boss Phase Ammo Refill | On | Defeating a boss phase or weak point restores 25% of each owned weapon's capacity. |
| Boss Weak-point Halos | On | Marks damageable boss regions with pulsing green halos. |
| Open Mechantis Final Phase | On | (For the boss fight right before the game opens up) After a mantis loses its weapons, its final weak point remains vulnerable during movement and attacks. |
| Halo-style Health | Off | After 30 damage-free gameplay seconds, restores missing health every five seconds over the next 60 seconds. |
| Ship Part Scanner | Off | Choose Audio, Visual or Audio + Visual guidance towards missing ship parts. Visual hints also need Show Guidance Text. |
| Show Guidance Text | Off | Shows ship-part scanner hints and guidance for the selected final Tribal requirement. This is mostly for debugging i would leave it off |

### Weapon shortcuts and throwables

Hold **Ctrl / L4** and press a weapon slot to assign the equipped weapon.
Press the slot alone to equip it. Slots 1-4 correspond to D-pad Up, Right, Down
and Left. Assignments are saved separately for each character.

Press **G / Y** to use the assigned throwable and return to your weapon.
To assign another, equip it, hold **Ctrl / L4**, then press **G / Y**.
Grenades, shurikens, fish food, plasma bombs, remote mines and cluster bombs can
be assigned; flares retain their normal weapon controls.

### Quick Saving

Press **F6 / L5** during gameplay to save a state, then **F9 / R5** to restore it,
including after restarting the game. Saving again replaces the manual state.
Ordinary game saves are separate. Restoring a state keeps your current QOL preferences. On Steamdeck you need to hold L4 and press the two buttons.


## PC controls

| Input | Action |
| --- | --- |
| WASD / mouse | Move and strafe / look and aim |
| Left mouse button | Fire |
| Right mouse button | Hold for fine aim / sniper scope |
| W / S while scoped | Adjust sniper zoom |
| Space | Jump / stand; skip cutscene when enabled |
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
Adjust sensitivity and inversion in settings, or remap actions in Bindings.

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

Jet Force Capricorn is an unofficial fan project. It is not affiliated with,
endorsed by or sponsored by Nintendo, Rare, Microsoft or the original game's
other rights holders.

Provide your own lawfully obtained ROM of the supported release. Do not upload
ROMs or extracted game assets to this repository or its issues. The original
game and its content belong to their respective owners.

## Project

Built with N64Recomp, N64ModernRuntime, RecompFrontend and RT64.

[Report a bug](https://github.com/riseio/Jet-Force-Capricorn/issues/new) and include
the build version and steps to reproduce it.
