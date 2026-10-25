# macOS

There is no native Mac build:
- The game is 32-bit x86 code that the loader runs directly on the CPU.
- macOS dropped 32-bit x86 programs in Catalina (10.15).
- Apple Silicon (M1 and later) isn't x86 at all.

## Intel Macs

Run Linux in a virtual machine (UTM, VirtualBox, Parallels or VMware Fusion) and follow the
normal Linux steps in the README inside it. Ubuntu 22.04 or newer works.

- **Graphics.** The VM's 3D support has to provide 32-bit OpenGL 3.3. Most VMs give either a
  slow virtual GPU or Mesa's software renderer (llvmpipe), so use a lower resolution:
  `XBOX_SCALE=1 dist/run.sh ~/games/spyro`.
- **Controllers.** Pass the controller's USB device through to the VM in its settings, or play
  on the keyboard.
- Booting Linux directly on the Mac, from a USB stick or a second partition, gives full speed.

## Apple Silicon Macs

Not supported. A Linux VM on an M-series Mac is ARM Linux, which can't run this x86 code
natively, and x86 emulation in UTM is far too slow for this. Rosetta for Linux translates only
64-bit x86 programs, and the loader is 32-bit.

## Getting your game files on a Mac

`setup-game.sh` runs inside the Linux VM. Share the folder holding your ISO with the VM, or copy
the ISO in.
