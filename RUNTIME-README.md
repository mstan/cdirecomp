# CD-i native player

Supply the player ROM used to build this executable. A disc is optional for the standalone system player:

```text
CdiRuntime player.rom --disc game.cue
```

The system ROM executes natively and boots the real CD-RTOS shell. Use its Play CD-I control to start an inserted disc. Disc images and firmware are not included.

Keyboard defaults: arrows/WASD move, Z/Enter/Space is button 1, X/Backspace is button 2. The mouse also drives the CD-i pointer. F11 or Alt+Enter toggles fullscreen. Esc quits or releases captured input. Game consumers may supply the shared launcher for media selection, control bindings and presentation/audio preferences.

Preferences and original in-game NVRAM live in SDL's per-user cdirecomp/player directory; the path is printed at startup. CDI_PLAYER_CONFIG_PATH selects an explicit isolated profile. Keep player.cfg and nvram.bin together and preserve them across updates. An example configuration is included; do not overwrite an existing player's choices with it.

Fit-to-screen presentation follows the window shape. Original 4:3 remains selectable. Fit scales the original picture; it does not expand a game's world or shooting field.

This is a research preview. Title compatibility and full-campaign/save-restoration acceptance are documented by each game consumer.
