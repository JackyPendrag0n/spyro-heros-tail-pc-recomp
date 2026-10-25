# Windows 10 / 11 (through WSL2)

There is no native Windows build. The loader runs the game's 32-bit code directly and relies on
Linux features to do it (see the README). On Windows it runs inside **WSL2**, the Linux that
Windows ships with. With **WSLg** (Windows 11, or Windows 10 22H2 with a current WSL), Linux
windows and sound appear on your normal desktop.

## The easy way: Install-Spyro.bat

1. Download this repo: **Code → Download ZIP** on GitHub. Unzip it anywhere.
2. Double-click `windows\Install-Spyro.bat`.
   - If you don't have WSL2 and Ubuntu yet, it installs them, which needs administrator rights.
     Restart if Windows asks you to, open **Ubuntu** from the Start menu once to pick a user name
     and password, then double-click `Install-Spyro.bat` again.
3. Pick your game when asked: your `.iso`, or press Cancel and pick a folder with the
   extracted files.
4. It builds the port inside Ubuntu, asking for your Ubuntu password once to install packages.
   The first build takes a few minutes.
5. Start the game with **Spyro A Hero's Tail** on your desktop.

Run `Install-Spyro.bat` again any time to update. Your saves are kept.

What it sets up, all inside Ubuntu:
- the port in `~/spyro-port`;
- your game in `~/games/spyro`;
- `~/spyro-port/play.sh`, which the desktop shortcut runs.

## By hand

### 1. Install WSL2 with Ubuntu

In PowerShell as administrator:

```powershell
wsl --install -d Ubuntu
wsl --update
```

Restart if asked, then open **Ubuntu** from the Start menu and create a user.

### 2. Get the port

In the Ubuntu window, either build it:

```sh
sudo apt-get install -y git
git clone https://github.com/JackyPendrag0n/spyro-heros-tail-pc-recomp spyro && cd spyro
./build.sh --deps
```

or use the prebuilt AppImage in `prebuilt/`. WSL usually has no FUSE, so run it with
`--appimage-extract-and-run`:

```sh
chmod +x prebuilt/spyro-heros-tail-x86_64.AppImage
prebuilt/spyro-heros-tail-x86_64.AppImage --appimage-extract-and-run ~/games/spyro
```

### 3. Your game files

Your Windows drives are under `/mnt/c`, `/mnt/d` and so on. For example, an ISO in your
Downloads folder:

```sh
./setup-game.sh "/mnt/c/Users/<you>/Downloads/Spyro A Hero's Tail.iso" ~/games/spyro
```

Extract into your Linux home (`~/games/...`), not onto `/mnt/c`, because loading from the
Windows drive is much slower.

### 4. Run

```sh
dist/run.sh ~/games/spyro
```

## Things to know

- **Graphics speed.** WSLg's GPU driver is 64-bit only. A 32-bit program like this one will
  most likely use Mesa's software renderer (llvmpipe), so it runs on the CPU. Lower the
  resolution to keep it playable:
  ```sh
  XBOX_SCALE=1 dist/run.sh ~/games/spyro
  ```
  The desktop shortcut already uses `XBOX_SCALE=1`. To change it, edit `~/spyro-port/play.sh`.
  The `gl:` line near the top of `xboxrun.log` names the renderer it got. Co-op is the heaviest
  case.
- **Controllers.** WSL doesn't see USB controllers by default. You can pass one through with
  [usbipd-win](https://github.com/dorssel/usbipd-win), or play on the keyboard (see
  [PLAYING.md](PLAYING.md)), which works out of the box.
- **Sound** goes through WSLg's PulseAudio and needs no setup.

If you have a spare drive, a real Linux install, or a dual boot, gives full GPU speed and
controller support.
