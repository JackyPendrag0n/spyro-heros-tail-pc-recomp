# Playing guide

## Controls

Any controller SDL recognises works: Xbox, PlayStation, Switch Pro and others. The buttons are
the Xbox ones (A = cross, B = circle, X = square, Y = triangle).

If no controller is connected, the keyboard works:

| Key | Button |
|---|---|
| WASD | left stick |
| arrow keys | right stick (camera) |
| I / J / K / L | d-pad |
| Space | A |
| Left Shift | B |
| E | X |
| Q | Y |
| 1 / 2 | White / Black |
| Z / C | left / right trigger |
| Enter | Start |
| Esc | Back |

## Playing as Red

Pick **Red** under *Character* in the debug menu, or turn on *Start as Red*. He plays in any
level at his normal size, with the game's own collision and camera, and stays Red across
levels until you pick someone else.

| Input | Move |
|---|---|
| stick | walk / run |
| A | a very high jump. Press A again in the air for a wing-stroke double jump, and keep holding it to **fly** for a few seconds before he glides down. |
| X (tap) | staff combo: up to four swings (forehand cut, backhand sweep, spin, leaping slam). Each hit hurts what your current breath hurts. |
| X (hold) | casts the current breath straight ahead from his staff, again and again |
| LT / LB (hold) | aim: over-the-shoulder view with a reticle. The right stick aims and the left stick strafes. Tap X to cast at the reticle, or hold X to charge and let go for a charged blast. He hovers while aiming in the air. |
| **Y** | **shrink bolt**: a bolt from his staff that shrinks the enemy it hits to 30% for 10 seconds. Jump on a shrunk enemy to squash it flat and finish it. The enemy blinks just before it grows back. |
| B in the air | ground pound |
| B held + X | tail-spin |
| B (hold) | dash: his galloping run, staff out front |

He also:
- reacts to hits;
- collapses and gets back up when he loses a life;
- goes flat when something squashes him;
- does idle gestures with his own voice lines.

Sparx flies beside his head. His staff leaves an ember trail and glows while he casts. The magic
meter shows how much magic he has.

The first time you play as Red, the port records his animations from the game's data. That
takes about half a minute, with "Getting Red ready" on screen, and you can play meanwhile. They
are kept in `redclips.bin` in the saves folder; delete it to record them again.

Red the boss, in his chamber, is untouched, so you can fight him as Red.

## Co-op

Turn it on with *Co-op* in the debug menu (2, 3 or 4 players, remembered between runs), or with
`XBOX_COOP=1 XBOX_COOP_PLAYERS=n`.

- The other players join next to player 1 once you are in the game and can move.
- **Two players:** the screen splits down the middle, with player 1 on the left.
- **Three or four players:** four quarters, in the order top left, top right, bottom left,
  bottom right. With three players the last quarter is black.
- Players 2–4 use the second, third and fourth controllers. With only one controller, player 2
  can use the keyboard.
- *Player 2/3/4* in the menu picks each player's character. Player 1 picks with *Character*.
- Players 2–4 sit out the Ball Gadget runs and the mini-games, which are built for one player.
  They come back next to player 1 afterwards.

## Debug menu

Open it with **F1**, or click both sticks (L3 + R3).
- Up/Down selects, A or Enter chooses, Left/Right changes a value, B or Esc goes back.
- The game gets no controller input while the menu is open.

| Item | What it does |
|---|---|
| **Warp to level** | Every level the game registers, named as in its own level table (1A Dragon Village … 4Z MechaRed), then the mini-games, then the developers' leftover test maps, marked [dev], which may not work. Left/Right picks the start point. Start or load a game first. |
| **Character** | Spyro, Hunter, Sparx, Blink, Sgt. Byrd, Ball Gadget, Ember, Flame or Red. This uses the game's own character-change route, as the GameCube *CharacterChanger* mod does. Not possible during cutscenes or conversations. |
| **Co-op** | Off, 2, 3 or 4 players. |
| **Player 2/3/4** | That player's character. |
| **Size** | 0.1% to 200%. Scales the model, the hitbox, walking, jumping and falling, and the camera. The change eases in over about 2 seconds. Attack ranges, talking distances and Sparx aren't scaled. The hitbox stops shrinking at 15%, and a fall through the floor is caught. |
| **Abilities & items** | Unlocks every ability and item, maxes every Moneybags upgrade, and keeps lives and bomb ammo full. Turning it off restores your own. Saving while it is on keeps the unlocked state in that save. |
| **Give 10,000 gems** | Also opens Moneybags' shop. |
| **Refill health** | Full health once. |
| **Invincible** | Health is refilled every frame. Pits and deadly water still cost a life. |
| **Noclip** (also F3) | Fly through walls and floors. Move with the left stick or WASD, go up/down with RT/LT or Space/C, and hold A or Shift to go fast. |
| **Squashed** | The character stays squashed flat until you turn it off. |
| **Free camera** (also F2) | Fly the camera around; the controls are the same as noclip, with the right stick or arrows to look. The game still culls from its own camera, so distant scenery can be missing. |
| **Modern camera** | On by default. The right stick turns the view all the way round and it stays there, and up/down tilts it. Off gives the game's original camera. |
| **Start as Red** | You are Red as soon as you start or load a game. |
| **Draw distance** | 1×, 2×, 4×, 8× or 16× the original. |
| **Frame rate** | 60 (original) up to 244. Gameplay uses real time, but some waits count frames and run faster, for example the logo screens. |
| **Show FPS** | A frame counter in the corner. |

## Options

Set these before the command, for example `XBOX_FPS=144 dist/run.sh ~/games/spyro`.

| Variable | Effect |
|---|---|
| `XBOX_WIDESCREEN=0` | 4:3 instead of 16:9. |
| `XBOX_SCALE=n` | Internal resolution in multiples of 480 lines. The default is your desktop's height. |
| `XBOX_COOP=1` | Co-op on. |
| `XBOX_COOP_PLAYERS=n` | 2, 3 or 4 players. |
| `XBOX_COOP_CHAR=n` | Player 2's character: 1 Spyro, 2 Hunter, 3 Sparx, 4 Blink, 5 Sgt. Byrd, 6 Ball Gadget, 7 Ember, 8 Flame, 9 Red. |
| `XBOX_COOP_CHAR3=n`, `XBOX_COOP_CHAR4=n` | The same for players 3 and 4. |
| `XBOX_COOP_FULLRES=1` | Draw each co-op view at full resolution. Slower. |
| `XBOX_FPS=n` | Frame rate. |
| `XBOX_DRAWDIST=n` | Draw distance multiplier: 1, 2, 4, 8 or 16. |
| `XBOX_SIZE=n` | Character size in percent, 0.1 to 200. |
| `XBOX_MODERN_CAM=0` | The game's original camera. |
| `XBOX_MOVIES=0` | Skip the movies. |
| `XBOX_VSYNC=1` | Use vsync instead of the built-in frame limiter. |
| `XBOX_VOLUME=n` | Master volume in percent (default 100). |
| `XBOX_NO_AUDIO=1` | No sound. |
| `XBOX_SAVE_ROOT=dir` | Where saves go (default: `xbox_hdd/` next to the game folder). |
| `XBOX_GFX=null` | Run without drawing, for crash testing. |
| `XBOX_TRACE=1` | Log every kernel call. The log gets large. |

## Reporting a problem

Include `xboxrun.log`, which is written next to `run.sh` or the AppImage, and say what you were
doing when it went wrong.
