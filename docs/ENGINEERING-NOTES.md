# Spyro: A Hero's Tail (Xbox, NA) – port notes

## Executable (default.xbe)
- Title ID 0x56550034 (VU-052), built 2004-09-21, XDK 5849, base 0x10000
- Entry 0x26d329, kernel thunk table 0x2ee180 (114 imports)
- Dev build path: c:\Game\Spyro\Code\Xb\Release\Spyro.exe  (engine: Eurocom EngineX + EuroSound)
- XDK libs live in their own sections (D3D, D3DX, XGRPH, DSOUND, XMV, XPP); game code + XAPI + CRT in .text

## Data files
Filelist.bin  – index (v7). 432 entries. Layout documented in tools/filelist.py.
                Names: self-relative pointers; byte i of name k = plain + 0xEA + k + i.
Filelist.txt  – debug text dump of the same index (validates 1:1 against .bin)
Filelist.cfi  – build cache: u32 7, u32 9, u16 2, u32 count(642), then records:
                u8 len, char path[len] (x:\ build paths),
                u32 f0 (?time), u32 0, u32 slot_size, u32 1, u32 timestamp,
                u32 length, u32 hash, u32 version, u32 flags
                642 = 432 shipped + 210 unshipped localised audio (spa/ita/ger/fre/dut, 722 MB)
Filelist.00N  – raw archives, files 0x800-aligned, no compression at archive level.
                slot_size - length = reserved slack (stale data may remain).
                concept.edb: 13.7 MB slot, 6 KB shipped -> check slack for old data.

Archive contents
- .000 (254 MB): character/object .edb, realm1*/realm2* levels
- .001 (265 MB): realm3*/realm4* levels, titles/text/panel, panel.bin, titles.bin,
                 publisherlevellist.txt, start of music
- .002 (268 MB): audio only (music, eng_ soundbanks, eng__streamdata.sfx 91 MB)

Hash high byte: 0x01 = .edb geometry/level, 0x21 = audio, 0x81 = raw data, 0xFF = test_sg.edb

## How the XBE finds files
- Archives: FindFirstFile/FindNextFile over the data dir, sscanf "FILELIST.%d",
  CreateFile(GENERIC_READ, share read, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING),
  (index, size) stored in table at 0x49fc20, count at 0x4a0420. Func ~0x261dc0..0x261f70.
- Movies: NOT in the archives. Path = <root> + "\Binary\_bin_xb\" + name + ".xmv",
  built at 0x1ba8b0 (virtual method, opens via 0x2323c0).
- Dev config: d:\Spyro\Albert\Spyro.ini (ref 0x17c9a)
- Dev defaults table at 0x347ea0: "d:\Spyro\Binary\_bin_XB", "...\BuildData\Filelist.bin"
- Audio + async file reads run on a command-queue thread (CMD_* strings).
- Saves: t:\$u\ (TitleMeta.xbx, SaveMeta.xbx, SaveImage.xbx)

## Tools
- tools/filelist.py  – parse Filelist.bin (+ validate vs .txt) -> data/filelist.json
- tools/extract.py   – extract from (split) archives, incremental, --slack
- tools/xbe.py       – XBE section map, string/xref finder, disassembly helpers

## Archive .001 (extracted 2026-10-15)
- 135/135 files OK. Magics: .edb 'MOEG' (GEOM LE), .sfx 'MUSX', panel/titles.bin 'GUIB'
- GEOM header: +0x04 hash, +0x08 version 240, +0x0C flags (= index flags), +0x10 unix time,
  +0x14/+0x18 file size, +0x38 (larger) probably in-memory size; relative-pointer tables from +0x54
- MUSX header: 'MUSX', u32 0xF001?, u32 4 (ver), u32 size, 'XB__' platform tag, timestamp
- Slack regions = tails of older, larger builds of the same levels (not whole files)
- publisherlevellist.txt lists dev-only Test_TL.edb, Test_PB.edb, model.edb (Model Viewer)

## Symbol identification (XbSymbolDatabase @20eced5, a local XbSymbolDatabase clone)
- data/xbsymbols.txt: 303 symbols. D3D8 124 fn + 33 var, DSOUND 91 fn, XAPI 39 fn + 12 var, XGRAPHC 4 fn
- data/kernel_imports.json: all 114 kernel imports named (from Cxbx-Reloaded KernelThunk.cpp)
  Nt 21, Ke 20, Rtl 12, Mm 11, Io 9, Hal 8, Ex 5, Xbox* 5 vars, Xc 4, Av 4, Ps 3, Xe 3, ...
  Interrupt/PCI/GPU (KeConnectInterrupt, HalReadWritePCISpace, MmClaimGpuInstanceMemory,
  Av*) are used by the D3D/DSOUND drivers; they go away once those libs are HLE'd.
- 32-bit toolchain: gcc -m32 works (gcc-multilib installed). Linux i386 glibc uses GS for
  TLS, so FS is free to emulate the Xbox KPCR/TEB via set_thread_area/modify_ldt.

## Loader (loader/, builds xboxrun) - status 2026-10-15
Run: `cd loader && make && ./xboxrun ../game`  (XBOX_TRACE=1 for full kernel log)
game/ = default.xbe, default.xbsym (symbol scan), Binary/_bin_xb/{Filelist.bin,.000-.002}
Emulated HDD: xbox_hdd/ (Partition0.img raw sectors, Partition1 = E:/TDATA/UDATA, Partition5 = Z: cache)

Boot sequence reached (headless, in order):
1. XBE mapped at 0x10000, 114 kernel imports bound (generated stubs + real impls)
2. Entry creates game thread (XapiThreadStartup 0x26f7cc, TLS 0x94 under StackBase)
3. XAPI: reads fake kernel PE header @0x80010000 (INIT reclaim check) -> needs map, done
4. XAPI maps D:, T:\TDATA\56550034, U:\UDATA\56550034, writes TitleMeta/TitleImage/SaveImage
5. XMountUtilityDrive: reads cache DB (Partition0 sector 4), claims partition 5, IOCTLs
   GET_DRIVE_GEOMETRY / GET_PARTITION_INFO, mounts Z:
6. Game main: heaps 0x700000 (7 MB) + 0x2500000 (37 MB "Large Data Heap") via MmAllocateContiguousMemoryEx
7. XInitDevices -> HLE input (hle.c), settings: video 0x30000, AV region NTSC, language English
8. STOPS: D3D device creation writes NV2A MMIO (0xfd001804) -> needs D3D HLE

Library call surface from game code (data/lib_callsurface.json):
  D3D 58 fns / 1146 sites (52 named), D3DX 10 / 429 (0 named), XGRPH 3 / 7, DSOUND 20 / 28 (all named), XMV 7 / 8
D3D inline usage: game writes D3D render-state shadow table (0x293468 dirty flags |= 0x800,
  render states 0x2934xx/0x2937xx) = deferred render states; NO raw pushbuffer writes found.
  => HLE plan: hook the 58+10+3 functions, read deferred-state shadows at draw time (Cxbx-style).

## Graphics/audio HLE + test build 1 (2026-10-15)
Disc layout (from the game's own requests): D:\default.xbe, D:\Spyro\Binary\_bin_XB\BuildData\Filelist.{bin,000-002},
  movies D:\Spyro\Binary\_bin_xb\{eurocom,NVUGlogo,...}.xmv. Loader falls back to a by-filename index of the game folder.
Boot needs: fake kernel PE header @0x80010000, raw disk Partition0.img (cache DB sector 4), disk geometry/partition IOCTLs,
  60 Hz vblank thread calling the game's VBlank callback (0x1cf9d0 increments [0x4703f0]+0x978/0x97c; loading waits on it).
D3D (XDK 5849) globals: IndexData 0x293464 (game's inline DrawIndexedPrimitive reads it), dirty flags 0x293468,
  TextureState[4][32] 0x293470, RenderState[166] 0x293670 (FOGENABLE=92, MULTISAMPLETYPE absent), g_pDevice 0x293908.
  Fake CDevice fields read by native helpers: +0x1a14 backbuffer surf, +0x1a18 frontbuffer surf, +0x196c msaa type.
  Unnamed hooks: 0x2838b0 kick+wait, 0x286330 GetScreenSpaceOffset (signature-checked).
  Game uses SetShaderConstantMode(0x11) and writes viewport constants c58/c59 itself; vertex programs output screen space.
Renderer: gfx/{d3d,vsh,psh,tex,gl}.c. VS microcode->GLSL, register combiners + fixed-function TSS->GLSL, all Xbox texture
  formats -> RGBA8, render-to-texture FBOs (game uses a 320x240..10x7 chain + 1024x1024 target), 60 Hz limiter.
Software GL test here: reaches 3D scene, ~420 draws/frame, 8 programs, 0 shader failures, 0 GL errors. Loader CPU cost ~5 ms/frame.
Audio: DSound HLE with real-time cursors, silent (SDL mixing next). Movies: not yet (XMV last).
Test build: dist/spyro-port-test1.tar.gz (glibc >= 2.34, 32-bit SDL2 + GL runtime).

## Test 5 fixes (black 3D scenes)
- X_VERTEXSHADERINPUT slots are 16 bytes (StreamIndex, Offset, Format, 4 bytes tess/pad), not 20:
  SetVertexShaderInputDirect was feeding garbage attributes -> black vertex colours across the scene.
- Back buffer / depth buffer now have guest memory; game samples the back buffer (LIN_X8R8G8B8 texture
  header on its Data) for its bloom chain -> resolved to the live FBO.
- Game pushbuffer: SET_TRANSFORM_CONSTANT_LOAD 0x1ea4 + SET_TRANSFORM_CONSTANT 0x0b80.. (vertex constants),
  SET_BEGIN_END 0x17fc + INLINE_ARRAY 0x1818 (inline geometry) now interpreted.
- Diagnostics: XBOX_DIAG=1 (per-target pixel counts, back buffer average), XBOX_DIAG_FRAME=N (per-draw trace),
  XBOX_DIAG_FRAG=diffuse|tex0|texcoord, XBOX_NO_DEPTH=1, XBOX_DUMP_SHADERS=dir.
- Open: ZENABLE=2 (W-buffer) treated as z-buffer; ~240/304 scene draws fail depth (may be legit occlusion).

## Test 6: rendering accuracy (frames now inspected here: XBOX_SHOT + Xvfb/llvmpipe)
Root causes of the "giant polygon" 3D:
- Vertex program memory: the game uses LoadVertexShader(handle, 0) (load program into slot 0
  without changing the declaration) and SelectVertexShader(handle, addr). Now emulated with a
  136-slot instruction memory; the executing program = slots from the start address to FINAL.
- SetVertexShaderConstantNotInlineFast count is in DWORDs (native code puts it in the method
  header), not float4s: 16 meant one 4x4 matrix, we were writing 16 registers -> c104.. garbage.
- ILU MOV is a vector move of the swizzled C operand (oD0.xyz = R11.xyz), not a broadcast.
Other fixes:
- Render-target colour textures had no GL_TEXTURE_MAX_LEVEL -> incomplete under mip filters
  -> sampled black (ground x shadow texture went black).
- With a vertex program active, 2D texture samples are projective (NV2A PROJECT2D); shadow
  texture coordinates carry q in oT1.w.
- SetPixelShader also sets D3DRS_PSTEXTUREMODES (RS 136) from def[54]; RS 54 is reserved.
- SetPixelShaderConstant converts to D3DCOLOR and writes PSConstant0/1[i] / final-combiner
  constant render states per the def's C0/C1/FC mappings (RS table is authoritative).
- Back buffer is created in the requested format (A8R8G8B8 -> LIN_A8R8G8B8 0x12): the bloom
  bright pass multiplies by back-buffer alpha (glow mask). X8 formats sample alpha = 1.
- Screen coordinates truncated to 1/16 px like the NV2A (D3D +0.53 offset hits pixel centres),
  w clamped to +-[2^-64, 2^64] (xemu behaviour).
- rdtsc virtualised at 733,333,333 Hz (timing.c): QPC hooked, `rdtsc;ret` helpers at 0x226ae0 /
  0x28d620 jumped, inline sites in the engine timer (0x2268ac..0x2269e1) -> ud2 + SIGILL emulation.
- Volume free space reported as 1 GB of 16 KB clusters (32-bit-safe) -> no "free blocks" warning.
Diagnostics: XBOX_SHOT=dir [XBOX_SHOT_EVERY=n, XBOX_SHOT_ALLRT=1], XBOX_INPUT_SCRIPT="frame:BTN[:hold],..",
  XBOX_DIAG_VTX=frame (CPU reference run of the vertex program per draw + state), XBOX_PROBE=frame,x,y[,rt]
  (draws touching one pixel, with depth), XBOX_TRACE_PUSH=frame, XBOX_DIAG_FRAG=t0..t3.
Verified here: title, menus, save UI (thumbnail), opening cutscene, Dragon Village gameplay.
Open: thin dark lines framing some UI boxes (unverified vs hardware), audio, XMV.

## Test 7: audio + W-buffer
- EuroSound mixes in software into 5 mono 16-bit DSound buffers (8 KB rings at 0x48fbd8 + i*0x2000,
  44100 Hz, looping, mix bins FL/FR/BL/BR/C each + LFE). audio_sdl.c mixes them to SDL stereo; with a
  live device the buffer play cursor advances by the frames the device consumed (g_audio_live), so
  the game's refill stays in step. Verified with SDL_AUDIODRIVER=disk: clean music spectrum, no clicks.
- ZENABLE=2 (D3DZB_USEW): fragment depth = perspective-correct w / zScale (+ZBIAS in LSBs), as xemu.
  The game's projection puts w = 16777.2 * eye z, i.e. linear depth across the 24-bit range; with
  z/w everything sat at 0.9999x and distant decals z-fought (the reported flicker). Measured flicker
  pixels in a static village shot: 733/frame -> 379/frame (remainder = animation).
- Testing shortcut: /tmp-style saved HDD + XBOX_INPUT_SCRIPT load path reaches gameplay at ~frame 3200.

## Test 8: widescreen, debug menu, free camera, frame rate
- Widescreen: XC_VIDEO flag 0x10000 makes the game render 16:9 anamorphically into 640x480
  (projection x scale = 0.75 * y). Presented at 16:9; render targets use separate x/y scales
  (x = 4/3 y) so pixels are square. Default internal resolution = desktop height / 480.
  Upscaled targets shift screen coords by 0.5/scale - 0.5 so the D3D +0.53 offset still covers
  pixel column/row 0 (else stale edges).
- Map registry: count 0x446000, Map* array 0x446008 (+0xdc .edb hash = map id, +0xe0 sub id);
  vtable[0] = GetClassDesc (mov eax, desc; ret), desc+4 = class name ("Map_DragonVillage").
  Map manager 0x43a9e8: GoToMap 0x1fd60 (thiscall: hash, start -1|0x4a00000N, ctx), QueueMap 0x1fce0
  (thiscall: Map*, ctx), map->[+0x120] SetStart 0x22cf0. Manager [+0x78] state must exist and have
  [+0x9c] == 1 (idle) - null on the title screen, so warping needs a started game.
  ctx = [0x42bb20] ? [0x42bb20] + 0x270 : 0 as the game's callers pass.
- Render context [0x4703f0]: +0x84 refresh (60/50), +0x8b4 projection, +0x8f4 view*proj (row-vector,
  depth-scaled), +0x934 camera position, +0x944 camera forward, +0x978/+0x97c vblank counters.
  Frame-rate vars 0x34ec40/44/48 (rate, half, target). dt = vblanks / refresh; one update needs
  vblanks >= refresh / target. XBOX_FPS raises refresh, target and the emulated vblank rate together.
  Some waits count frames (logo screen gap 0.5 s at 60 -> 0.125 s at 240).
- Free camera: renderer-level. Per draw M' = X^T M (c96..c99), X = VPg^-1 Vf Vg^-1 VPg with Vg/Vf
  look-at matrices from the game camera / free camera. Applied only to perspective transforms on the
  back buffer (not HUD, not the shadow texture). Game culling still uses its own camera.
- Debug menu (F1 / L3+R3) drawn by gfx/overlay.c (font atlas from DejaVu Sans Mono, tools/gen_font.py).
- Test hooks: XBOX_WARP=frame:hash[:start], XBOX_FREECAM_AT=frame, XBOX_LIST_MAPS=1,
  XBOX_SHOT_WINDOW=1 (capture with overlays), XBOX_SHOT_FROM/TO, script buttons L3/R3/L3R3.

## Test 9: cheats and noclip (loader/debugmenu.c)
- Inventory block 0x43cd10: +4 health (0xa0 full; refill = thiscall 0xcf2a0(0x43cd10, 0xa0)),
  +8/+0xc gems (both 10000 = the game's own "Lots of Gems"), +0x28 (0x43cd38) ability/item bits.
- Item counters 0x43cd20..0x43cd33: five groups {count, max, cap, upgrades bought}. Shop (0x1a5050)
  refuses a purchase with 3 when count+n > max (consumable full) and 2 when max+n > cap (fully
  upgraded); purchase handler 0x1a5490 adds item+0x16 * n to max and n to upgrades.
  New game values: 00 01 03 00 | 00 0a 32 00 | 00 04 14 00 | 00 14 64 00 | 00 02 0a 00.
- Unlock = SetFlag 0xcc370 (ecx 0x43ce2c) on 0x44000001/03/25/9c/9d, OR 0x7c2f5 into 0x43cd38,
  max = cap and count = max per counter group, re-applied every frame while on.
  The retail "All Abilities"/"All ..." debug commands are empty stubs.
- Player [0x44c704]: +0x118 position (world matrix translation, rows at +0xe8), +0x128 copy,
  +0x4b4 collision centre (position + ~1.7 up), +0x630 / +0x754 other derived points.
  In the air Spyro glides at a constant sink rate (~0.15/vblank); no accumulating velocity, so
  noclip simply overwrites the three positions once per frame at the Swap hook.
- Noclip movement basis = game camera forward (rctx +0x944) flattened to the ground plane.
- Tests: XBOX_CHEAT_AT=frame:mask (1 unlock, 2 gems, 4 health, 8 noclip, 16 invincible),
  XBOX_NOCLIP_LOG=1, XBOX_NOCLIP_PROBE=frame (per-field change dump).
  New-game input script that survives boot-time variance: START at t, A at t+50, every 100
  frames from 1300 to 3300 on an empty save root (pause/continue pairs are harmless in game).

## Test 10: character change, squashed, draw distance (loader/debugmenu.c)
- Player handler [0x44c704] (GC offsets - 0x30 / - 0x48): +0x548 type (1 Spyro 2 Hunter 3 Sparx
  4 Blink 5 Sgt Byrd 6 Ball Gadget 7 Ember 8 Flame), +0x54c move flags, +0x550 state flags,
  +0x7ec mode (enum as in the GC CharacterChanger mod's player.h), +0x7f4 requested mode,
  +0x7f8 mode history[8] / +0x818 index. SetMode = thiscall 0xc1be0(mode, flags, arg);
  per-mode enter/update/exit = vtable +0x1ec (0xddf40, switch on mode).
- gPlayerLoader 0x3498ac {m_LastLoad, m_SoundBank}; GetPlayersFileList 0xc1a70 (7, 8 -> Spyro's);
  PreLoad 0xc42a0, IsLoaded 0xc1ad0 (thiscall), DeLoad 0xc42c0; ChangePlayer 0xc4c40 (cdecl
  player, inCutscene). Player setup block 0x43cd6c (pos/rot, +0x24 player, 0x43cd94 / 0x43cd98
  reset by ChangePlayer: restored afterwards like the GC mod does).
- Squashed (mode 67) handler 0xebc90: enter plays anim 0x9000124; update eases +0x6d0/6d4/6d8
  (-> item +0xec/+0xe8/+0xf0 scale) to 0.25/1.25/1.25 until anim event bit 8 in +0x7ac, then back
  to 1 (and vt+0x214 checks health). Item [pl+4]: +0xe8.. scale, +0x7e = 0 marks matrix dirty.
- Camera: EXCamera +0x4c near (0.001) +0x50 far (1000), applied by virtual 0x1e0460 (hooked:
  `sub esp,0xe0` -> stub). Projection is infinite-far style but the far plane is the cull distance
  (far = 30 cuts the scene at 30 units). W-buffer depth scales with far (VP is depth-scaled).
- Objects: SE_Trigger vtable 0x2f1b18; IsInside/IsOutsideActiveRange 0x291a0/0x291c0 call
  0x290e0(range^2, outside) which tests 3 scan points at 0x4464e0 (stride 0x18). Hooked to
  scale range^2.
- Test 11: squashed handlers per class (vt+0x1ec switch, case 67): Spyro/Ember/Flame 0xebc90,
  Hunter 0x104520, Blink 0x122130; Sparx/SgtBird/BallGadget have none. Anim events: +0x7a8 this
  frame / +0x7ac accumulated; bit 8 = flat pose reached (peel starts), bit 1 = anim end (-> mode 1).
  Hold = clear bits 1|8 each frame + animator clock stopped: animator = [item+0x13c]-4 (vtable
  0x33a3f8), Update virtuals 0x1fd090 / 0x1fd100 hooked to pass dt = 0 for g_frozen_anim.
- Test 12: test 11's animator hooks were wrong (0x1fd090/0x1fd100 are SetTime, not the clock).
  Real clock: track advance 0x217680 adds dt * animator +0xf8 (speed) * [0x34ec78]; track (node-4,
  node = animator +0x11c) +0x80 = normalised progress. Squash anim 0x9000124 lies flat ~0.1-0.5.
  Hold = speed 0 once progress >= 0.30 (armed only after item +0x178 == 0x9000124 and progress
  seen < 0.30, since the new track can start a frame late, e.g. Hunter); off = speed back + bit 8.

## Test 13: character size (loader/debugmenu.c, "character size")
- Model scale item +0xe8/+0xec/+0xf0. Physics = [item+0x148] (XItemPhysicsPlayer, vt 0x318bb8;
  Spyro 0x31c058): shape +0x160 centre (0,1,0), +0x170 radius+0.1 (1.1), +0x19c type (1 = capsule,
  radius at +0x180); defaults set at 0xccd3e. Collision = vt slot 0xccd80: sweeps from +0x140 (last
  resolved pos) to item +0xc8 in steps of radius*0.25/0.75, writes the result to +0xc8/+0x140.
  Hooked at entry (sub esp,0x1d4): the delta is scaled there -> movement scales with the hitbox.
- Item +0xd8 is rotation (not velocity); item->Move(vec) 0x1b5d0 adds to +0xc8 (gravity/pushes only).
- Ground contact needs the sphere to reach 0.1 below the feet (keep the +0.1 unscaled) and does not
  survive a sudden big change of the sphere (ease <= 3 %/frame). Sphere radius floor 0.15.
- Low frame rates (test machine 6-15 fps) still let small/large spheres sink through floors at
  some spots; per-frame step capped at half the sphere, plus a put-back if it falls > 8 units.
- Camera: in the camera hook, eye +0x1c and look-at +0x2c of a camera aiming within 6 units of
  the player are scaled about the feet; near plane +0x4c * size.
- Tests: XBOX_AUTOSTART=1 (START/A until the player is controllable), "+N" frames relative to that
  in XBOX_INPUT_SCRIPT / XBOX_SIZE_AT, XBOX_SHOT_REL=1, XBOX_SCALE=0.25 (~40 fps on llvmpipe).

## Test 14: play as Red (loader/debugmenu.c, "play as Red")
- XSEItemHandler_Red vt 0x317d58 (class desc 0x317d30: size 0x598, create 0xbe6c0). Only exists in
  3Z Red's Chamber (map 0x010000e5); found by scanning committed arena pages for the vtable.
- Possession: vt slot 16 (Update, 0xc0330 = his AI) -> stub that skips the AI for the possessed
  handler but must return 2 - any other value makes the item skip its animator too (anims froze).
- Spyro is the invisible driver (size x2.5 for hitbox/camera); Red's item copies the driver's
  position/rotation each tick (+0x7e = 0). Anim: animator ([item+0x13c]-4) PlayAnim 0x1fd240
  (thiscall cur, new, 0x9000004) + item +0x178 = new (same as NPC helper 0x77540).
- Red's animset (36): 004 idle, 00d roar/wings, 054 collapse, 189-191 idles/gestures, 194/195 staff
  lunges, 199 fireball, 19f/1b3/1f2 wing flourishes, 1a3 walk, 1a4 run (prowl), 1a5/1b2/1b7 low
  moves, 1b8 jump, 1b9/1ba flying high, 1c7-1cc staff attacks (1c8 ice beam). XBOX_RED_GALLERY=1.
- Driver inside Red's body gets thrown around (mode 34 = knock-back, velocity +0x8f4..). Parking
  Red's item away and moving only his matrix (+0xb8) made him vanish (bones are built from the
  item at update). Instead SetMode(34) is refused while possessed (SetMode hook at 0xc1be0).

## Test 15: Red anywhere, Red's own controller, pose layer (debugmenu.c "Red's own controller", redpose.c)
- bossred.edb (0x010000db) is 0xb4800 bytes in Filelist.bin but its archive slot holds the full
  0x15f000: section 1 (anim data, 0xb4800..) follows in the slot. (tools/edb.py reads the lists;
  full file extracted from the slot.) Layout of lists per eurochef (github.com/eurotools/eurochef).
- File manager 0x4375a0: Load 0x1f860 (thiscall list*, mode 1 load / 2 unload; list = 0-terminated
  file hashes), IsLoaded 0x1f990. Player file lists: GetPlayersFileList 0xc1a70.
- Item class table [0x4464d4] count [0x4464d8], 0x28-byte entries {class 0x41.., 0x42000001, alloc
  0x2d260, create, desc, ...}; Red = 0x4100012e (create 0xbe720 -> 0xbe6c0). Spawn: 0x28e60 thiscall
  on 0x4464e0 (class, 0x42000001, pos*, rot*, 0) - the player factory 0xc2ee0 does the same.
- Red's init 0xbe850 looks up map 0x010000e5 (push at 0xbe910) and touches its live data: outside 3Z
  that lookup is pointed at no map during the spawn (crashed at 0x1e61f3 otherwise).
- Controller replaces the AI (vt slot 16 stub calls red_control, returns 2). Root motion 0x79bf0
  (handler, flags 1 = translate / 2 = rotate) is zero for his walk/run clips, so movement is a set
  pace (walk 5, run 13, air 9, flying 14 units/s) with acceleration. Movement is added to the player
  item position; the player's collision sweep (0xccd80) resolves it; Red is placed on the result.
  The game's pad only gets A, B, Start/Back and the right stick (input_sdl g_red_capture).
  Yaw convention: forward = (-sin yaw, -cos yaw); camera screen-right = (fz, -fx).
- Flying: capsule physics +0x7c = vertical speed (0.25 climbing).
- Pose layer: animator 0x1fce30 samples clips; Red's clips decode straight into the model's bone
  records ([animator+0x104] out object: +0x28 records of 0x48 bytes, +0x2c count, +0x38 4x4 model-space
  matrices (row-major, translation row 3); record +0x0c matrix ptr, +0x18 translation, +0x28 quat
  xyzw, parent-relative). Hook: 'call 0x2523c0' at 0x1fcf6e (end of 0x1fce30) -> red_pose_post edits
  the records and clears out+0x10 / out+0xe (matrices stale; 0x2119f0 does the same). Only animated
  bones are rewritten by the decoder, so the clean values are kept and restored before each edit.
  (0x2119f0, the blended-pose copy at 0x1fcf65, is not used for Red's single clips.)
- Red skeleton (54 bones, bind positions in the skin): 0 root, 1 staff, 2 pelvis, 3-5 spine, 6-10
  neck/head, 11-14 face, 15-20 / 21-26 wings, 27-30 / 33-36 arms, 31-32 chest, 37-40 / 50-53 legs,
  41-49 tail. Model space +y up, -z forward.

## Test 16: Red as a character (character select), no capsule, boss left alone
- Red's body is an animator made from entity 0x0300005e in bossred.edb: res = 0x1c3fa0(file)
  (cdecl), animator = 0x1fdfb0(res, entity) (cdecl), then as his boss init does: 0x1fdf20(anim, 2),
  0x1fa000(anim, layer 0..2, 6, 1), 0x1e7810(anim, 60.0f), anim+0x7c = 0x13, scale +0xe8..+0xf0 = 1.5,
  0x1fd3e0([anim+0x190], 1). (0x77260 = handler SetModel wrapping these.)
- Item animator list: head [item+0x13c] (node = animator+4; node[0] prev, node[4] next, appended
  at the tail by 0x1f5e90). Attach 0x1e5d80, Detach 0x1e5db0, Destroy 0x1e5dd0, Find-by-hash
  0x1e5e00 (thiscall item, animator). The player code uses the head animator (Spyro's).
- Animators draw only with bit 0 of +0x10 (item draw loop 0x1e6f60 -> animator vt+0x14).
- "Red" in character select = a Spyro-class player (type 1) with Red's animator attached and
  Spyro's left in place but not drawn (scaled x3.5 so breath etc. leave from about Red's head).
  Physics/hitbox/camera scaled x2.2 through the size code (model scale left alone). Red's clips are
  picked from the player's mode and speed; modes: 26 breath -> 199, 29 charge -> 194 (alternating
  mirrored), air 5/8/10 -> 1b8/1ba + wing beats. D-pad: roar, jab, taunt (synth), staff spin.
- The old possession (AI skipped, capsule driver) and the Play-as-Red toggle are gone, so the 3Z
  boss (and his health bar) is untouched; no Red handler is spawned elsewhere.

## Test 17: Red properly animated
- Why he "floated": his clips live in bossred.edb section 1, which a plain file load doesn't bring
  in. The track advance (0x217680 -> 0x2148b0 -> 0x2144a0) finds each clip through the file's anim
  list runtime pointer, which stays 0 without section 1, so every clip sat on frame 0. Fix: load
  list { 0x010000db, 0x08000001, 0 } - a 0x08xxxxxx entry after a file hash asks the file manager
  (0x1f860, 0x1f3b0 type-3 job) for that file's section N. The item advances every attached
  animator itself (0x217820 per tick), so no extra clock call is needed.
- PlayAnim 0x1fd240 (thiscall animator, from, to, fallback-from): plays 'to' through the anim-mode
  script for from->to. His one-shot clips return to idle by themselves; asking for the idle mid-clip
  leaves the clip frozen on its last frame, so the idle is never forced.
- Red has no walk/run clip (gallery with working clips: attacks, gestures, flight clips that lift
  him out of shot, 1b8 a long take-off crouch). Locomotion is synthesized in redpose.c on top of
  the idle: stepping legs (hip swing, knee/ankle fold on the swinging leg), hip bob and twist, arm
  swing, wings folding with speed, tail wave; phase advances with distance (stride 3..6 units), so
  the feet keep pace. Plus forward lean with speed, roll into turns (yaw rate), landing squat,
  idle breathing, and the hover pose (wing beats, legs tucked) in the air.
- Tests: XBOX_AUTOSTART now never declares "gameplay began" inside its START press window (twice a
  test had started paused).

## Test 18: transitions, jump/glide, melee, staff in hand
- Clip progress: the newest track is the layer list TAIL ([animator+0x120]), not +0x11c.
- No more getting stuck: the engine can't cut his clips short, so while one is still running and
  nothing wants it (you moved / jumped) the pose layer blends to a captured idle pose instead
  (g_idle_ref, re-captured whenever he's been calm for 0.6 s). Every state change also cross-fades
  from the last shown pose (0.06-0.25 s).
- Air (made for him): rise (vs > 1: wings swept back, legs trailing, arms up), glide (slow descent:
  wings spread wide with a slow beat, body flat, head up, legs trailing), fall (vs < -10: half-open
  wings flapping hard), a full wing stroke on a double jump (upward kick in vs), landing squat.
  Mode 10 is the landing state, not air. Take-off speed raised to 9 (physics +0x7c).
- Square (X) = staff swing (forehand, backhand on a quick second press), upper body only so it
  works on the move; X is withheld from the game and a 6-frame B (charge) tap is sent at the
  strike so the swing actually hits.
- Staff (bone 1, child of the root): its transform relative to the nearer hand (30 or 36) in the
  clip pose is kept after all layers, with full FK (positions + rotations).

## Test 19: gestures, glide, sounds, Red on Hunter
- Gestures have fixed lengths (roar 2.4 s, jab 1.1 s, taunt 3.0 s, spin 1.6 s); outside one the
  pose layer always blends to the captured idle (g_pose_base_w = 1), so a clip the engine keeps
  holding can never show. Mode 10 counts as air only while |vs| > 0.5 (it is also used briefly at
  a double jump / glide), which stopped the gait running in the air ("air walking").
- Sounds: audio_sdl.c sfx_swoosh(peak_hz, dur, vol, pan) - band-passed noise with a swept centre,
  4 voices mixed after ds_mix. Staff swing 1350/1750 Hz, spin every 0.27 s at 2100, jab 1900,
  wing stroke 380, flip 420, glide beats 330 (3 rad/s), landing thump 160 (after > 0.3 s air).
  The game's own sfx (0x1542e0 2D / 0x154300 3D) take a hash we have no names for.
- Hunter (type 2) modes: X -> 78 bow shot (anim 0x9000098), B -> 81 roll (0x90000b3), Y -> 82
  (0x90000dc), A -> 8, second A -> 11 flip, 10 land, White/LT -> 32 aim (over the shoulder).
- Character "Red (Hunter's moves)" (index 10): g_redchar_base = 2, Red attached to Hunter instead
  of Spyro (RED_SIZE 1.5, Hunter scaled 2.0 underneath). 81 roll -> lunge clip 0x090001a4,
  11 flip -> synthesized somersault (bone 2 rotated 2*pi about x with a tuck) + wing stroke,
  32 aim -> staff arm raised, and Red's draw bit cleared while aiming (the over-the-shoulder
  camera sits inside his body). The melee hit is sent as an X (bolt) tap instead of B.

## Test 20: one Red - high jumps, glide, dash, swings
- Removed: the D-pad gestures (and with them every use of his engine one-shot clips) and the
  Hunter-based variant (character 10). His idle clip plays; everything else is made in redpose.c.
- Jump: Spyro's take-off holds vy (physics +0x7c) at 6.78 while A is held (game overwrites it each
  frame), then gravity runs from the stored value. On the first frame after that hold vy is raised
  by (13 - 6.78) - a tap rises ~5 units instead of ~1.3. A second press in the air sets vy = 12 (a
  wing-stroke jump, kick pose + sound). Mode 10 is the landing frame only, not a double jump.
  The game's glide (A held while falling) caps the fall at 4/s; it overwrites vy, so it is kept as is.
  XBOX_RED_JUMP / XBOX_RED_JUMP2 override the two speeds, XBOX_RED_VY=1 logs vy per frame.
- Camera: the game's follow camera hardly rises for a jump, so Red left the top of the screen;
  size_camera lifts eye and target by 0.75 * height above take-off (g_cam_lift), easing back
  after landing.
- Dash (B held, modes 29/17): a made pose (charge_pose: pitched forward and low, head up, wings
  swept back and folded, staff arm forward, other arm back) blended by weight with cross-fades,
  over the run cycle. A charge begun < 0.5 s after a swing is the swing's hit (the B tap) and
  shows no dash pose.
- Square swings work in the air too (no B tap there).

## Test 21: modern camera, Red's own clips for dash/jump/air, Red's voice
- Clip scrubbing: track +0x80 = play position 0..1 (the advance 0x217680 adds speed*dt; +0x84 =
  previous; +0x20 flags: bit 0 loop, bit 4 end callback 0x217320). Writing +0x80 scrubs. ONLY when
  the animator has a single track ([anim+0x120] == [anim+0x11c]): right after PlayAnim the tail
  is a short-lived blend entry smaller than 0x84 bytes, and +0x80 there corrupted the next heap
  object (crash at 0x799d6 in an NPC's update, [esi+0x10] = our float).
- XBOX_RED_SCRUB=clip,clip,.. [XBOX_SCRUB_SECS=n XBOX_SCRUB_NOLIFT=1] steps through his clips.
  Findings: 1a4 = low galloping run (24 fr), 1a3 walk cycle, 1ba = take-off (crouch, spring with a
  big wing stroke, then flying; pelvis lifts 1.4 -> 8.9), 1b9 = flying loop (pelvis ~9), 1b8 =
  landing (flare, then crouch, ends in the idle), 19f/1b3/1f2 wing flourishes, 194/195 staff
  lunges, 054 collapse. Flight clips lift the pelvis (bone 2) - taken out by g_pose_nolift.
- Red now: dash = 1a4 looped (position moved back near the end); jump and double jump = 1ba;
  air = 1b9 looped; out of the air 1b8 is played so the engine returns to his idle (flight clips
  loop forever; asking for the idle mid-clip freezes it). While a clip is wanted base weight 0;
  otherwise the captured idle covers whatever the engine is finishing (capture waits 1-1.6 s).
- Voice: eng_sb_realm3z.sfx (3Z sound bank, MUSX "XB__") read from the Filelist archive
  (redvoice.c). Bank: +0x20 sfx table {hash 0x00f0xxxx, offset} -> record (u16 count at +16,
  variants {u16 sample,..} 8 bytes each); +0x28 sample table (0x20-byte entries: offset, size,
  rate); +0x30 data. Codec: 32-byte blocks: s16 sample, u8 step index, u8 letter 'A'..'Z', 28 bytes
  = 56 IMA nibbles, high nibble first. Red's clip events (in the anim headers of bossred.edb)
  reference 0x1af00231..239; his anims' events: 11/14 -> 231 (tonal hum, fireball charge),
  12 (staff lunge) -> 233 + 238, 17 -> 233, 15/24 -> 234, 16 -> 234 + 235, 19 (collapse) -> 232
  + 235, 21/29 -> 239, 22 -> 235. Spectra: 238 = short noise bursts (staff whoosh), 233 = his
  yell, 234 = effort grunts, 239 = short voiced cries (9 variants), 232/236/237 long voiced.
  Used: jump 234, double jump 239, swing 238 (+233 every third / backhand), dash 233.
  Mixed by audio_sdl.c sfx_pcm (4 voices, linear resampling). The game's own events don't play
  them outside 3Z (sound starts 0x1cd730 2D / 0x1cd890 3D - XBOX_SFX_LOG=1 logs hashes).
- Modern camera (debugmenu.c modern_camera, menu toggle, XBOX_MODERN_CAM=0 to disable): the
  game's view camera (the follow camera looking down at the player; a second one at eye height
  is ignored) keeps doing the following and the wall avoidance. Right-stick x is passed through
  while held (+x turns it to lower yaw); when released the yaw is held by handing the game a
  right-stick x = -5 * (want - yaw), so it doesn't swing back; after 1.5 s of running with the
  stick untouched the game's own following returns. Right-stick y is ours: the eye is tilted
  about the look-at point (pitch -0.25..0.95 rad), easing back while running.
  A first version that placed the eye itself (free orbit) went through walls and fought the
  game's stick handling - the game moves the player relative to the rendered camera already.

## Test 22: Red's clips recorded and played by the port; combo, magic, flight, reactions
- Clip baking (redpose.c bclip, debugmenu.c bake_tick): each of 34 clips is started with PlayAnim,
  then its track position (+0x80) is written one step at a time; the animator's sampled pose is
  captured in red_pose_post before any changes, with the position it was sampled at. The engine
  advances the position by one frame before sampling, so that measured step is subtracted from
  what is written. If a clip's own script switches to another clip (the flight loop does), it is
  started again and recording continues past the last captured position. Only on a single
  settled track (see test 21: writing +0x80 on a blend entry corrupts the heap). Up to 40 poses
  per clip; saved to <save folder>/redclips.bin ("RCL1", NB, count, bclip[]). First run takes
  ~30 s at 60 fps; his captured idle covers it ("Getting Red ready").
- Playback: two layers (g_rcl): L0 locomotion (walk 1a3 at speed/6, dash 1a4 x1.25, take-off
  1ba, flying 1b9, landing 1b8), L1 actions (combo 1c7 -> 1cb -> 194, cast 1c8 held between
  0.40-0.62, hurt 1a1 0.12-0.62, collapse 054 then get-up 011, gestures 189 18a 18e 18f 190 191
  00d 1b3). Upper-body mask when moving. The engine only plays his idle now.
  Flight-clip lift is removed by moving the pelvis AND the staff (bone 1, child of the root).
- Flight: after the double jump, A held: A is hidden from the game (no glide), vy = 1.8 for up to
  2.7 s, then -1.5; stamina 3.5 s refills on the ground.
- Magic: square held > 0.3 s passes X to the game (Spyro's breath, mode 26) while he casts.
- Reactions: health (INV_BASE+4) drop -> hurt + cry 239; health 0 or lives (INV_BASE+0x10) drop
  -> collapse + 232. Tests: XBOX_HURT_AT / XBOX_KILL_AT (frames after gameplay began).
- Effects: the staff bone's matrix (record +0xc, model space, rows) -> world with the animator
  scale (+0xe8) and the item's position/yaw (item +0x118 is not a matrix for the player); the
  game's view-projection is rctx +0x8f4 (rows, screen y = 0.5 - ndc_y/2); camera at rctx +0x934.
  Drawn with the overlay (ov_glow) inside the game picture rect (g_pres_rect). Tip = bone origin
  + 1.1 x row 1. XBOX_FX_CAL=1 draws calibration markers. XBOX_SHOT_WINDOW=1 screenshots the
  window (with overlays) instead of the game's back buffer.
- Red's own particle effects (bossred.edb particle list, 0x1100000c..) are not referenced by any
  code in the exe - they're fired from his anim/script events, which only run for the boss.
- Camera for Red: follow-camera eye pulled out x1.18 and both raised 0.8 x size.
- Start as Red: portcfg.txt "start_red=1" (menu toggle; XBOX_START_RED=1 overrides).
- Unknown player modes (no Red animation) fall back to the flying loop off the ground and are
  logged once ("red: player move N has no animation of his").
- Hit tap (B) for the swing now lasts 0.1 s of real time, not 6 frames (works at any fps).

## Test 23: walk, cast pose, squash, Sparx, idle voices
- Walk: back to the made upright stepping (gait layer); his walk clip 1a3 is a hunched prowl that
  had to run at x2+ for Spyro's run speed (looked frantic). Clip still recorded, unused.
- Cast (square held): cast_pose in redpose.c (staff arm overhead, other arm forward palm out,
  chest up, wings open 0.3 rad + breathing, tail curl, slight rise); g_pose_cast eased. The 1c8
  clip is no longer used for it. XBOX_CAST_WINGS=spread,tip to tune.
- Staff effects now use our own FK of the final pose (g_staff_head: staff bone origin + 2.0 along
  the staff's own axis, taken as the direction that points up in his idle) - the engine's bone
  matrix (record +0xc) was a frame behind and in a different frame of reference.
- Squash (mode 67): the game itself scales the player item to 0.25 x 1.25 and Red is drawn with
  the item, so only the pose (squash_pose: limbs and wings spread flat, quiver) and sounds are
  ours; on release a spring on his animator scale (+0xe8..) gives a pop with a wobble.
  XBOX_SQUASH_AT now takes +N (frames after gameplay began).
- Sparx: XSEItemHandler_Sparx (desc 0x314c68, size 0xc78, create 0xa5600, vtable 0x314cb8)
  update = slot 16 0xa5830, wrapped: the drawing offset (4.6 to Red's right, 6.8 up, eased as he
  turns) is removed before Sparx's own update and added after, so his following logic never
  sees it. XBOX_SPARX_OFF=side,up; XBOX_SPARX_LOG=1. Sparx's state machine: 0xa43c0 (+0x4a4, 13
  states).
- Voices: his bank's voiced lines 236/237 (long, very tonal - laughs/taunting calls) and 232 go on
  idle gestures (2 in 3). Spoken dialogue is in eng__streamdata.sfx: 1385 streams (table at
  0x800 of 0x15a4 bytes, data from 0x2000, each with a 0x1000 header then the same 32-byte IMA
  blocks; 1276 mono, 109 with 3 channels), indexed, with no speaker labels; the game's code only
  references 4 stream ids directly (0x1b000017/2c/2f/c750) - lines are started from data.

## Test 24: squash pose, staff glow, snappier control
- Squash pose: limbs out sideways in the standing frame, then the pelvis tipped forward 1.5 rad
  and lowered (belly down); his own extra flattening x0.42 of the game's 0.25 (animator scale).
- Staff crystal: a point 1.7 above the staff origin in the staff bone's own frame (captured in his
  idle), from our FK. While casting the staff is rotated about the hand to stand upright.
- Control: ground speed x1.3 (g_move_xz, applied to x/z in the collision sweep scaling); turn
  assist: yaw eased toward the stick direction (camera-relative, rctx +0x944) at 10/s on the
  ground, 5/s in the air; gait/bank respond faster. XBOX_RED_SPEED, XBOX_RED_TURN.

## Test 25: jank, squash wings, boss voice
- Jank at full speed: the turn assist's wanted yaw was ~1.4 rad off the game's own facing (wrong
  camera-direction convention), so it yanked the yaw against the game every frame. Off by default
  (XBOX_RED_TURN to experiment); banking toned down.
- Squash: limb splay signs were inverted (+x limbs need +z rotation to swing out); wings z -1.4 +
  y 0.7 (fanned apart). XBOX_SQ_WING=z,y,x.
- Voice: in his levels (boss handler updated within 1 s) his lines go through the game's own
  0x154350 (cdecl hash 0x1af00000|id, item) - same as his boss code, positioned and at the game's
  levels. Elsewhere it returns 1 but his bank isn't loaded (silent), so ours plays (gain 0.55).

## Test 26: Red's aimed casting, turn assist fixed, staff combo
- Spyro's first-person aimed shot: mode 32 (White / LT); X there calls breath object slot 1
  (thiscall obj, 1, 0) - fire 0xe3370, ice 0xe3a80, electric 0xe4090, water 0xe5670. Breath
  objects in the player handler: +0x13f0 fire (type 1), +0x1460 electric (2), +0x1428 ice (4),
  +0x1498 water (8); current type [0x43cd10]. Shot spawns from camera controller
  [[0x42bb18]+0x4b8] +0x284 eye toward +0x294 look-at; speed from obj +0x24/+0x28 (both 0 outside
  mode 32 -> NaN: set 1/1); cooldown obj +0x2c = 20.0; camera shake 0x14b4b0 (cam +0x3a0..0x3b4,
  state 2); ammo bytes 0x43cd24/25.. (fire/ice/elec, movsx byte). Projectiles: fire class
  0x41000035 (vtable 0x319098, update slot 16 0xda9f0, life +0x410 from 2.0, state +0x40c, 3 =
  exploding), ice 0x4100003d, elec 0x41000059. Spawn 0x28e60 then velocity 0xd3cc0 (thiscall
  handler, vec4*: phys +0xe8/+0xf0 per frame, +0x7c = y * 60/s) - direct calls in all three shots.
- redcast.c: Red's cast = slot 1 with eye/look-at swapped for staff crystal / aim point, cooldown,
  ammo and shake restored. 0xd3cc0 hooked (stub re-executes 7 bytes): during our casts the handler
  is recorded, velocity replaced (straight, 1.6/frame), and its class's slot 16 wrapped (generic
  table) to hold the velocity every update (no drop) and, for staff hits, short life + hidden
  (item +0x10 bit0). A staff hit that times out has its impact effect (0x04000068 through
  0x376d0) swapped for a missing id (0x040001fe in bossred = nothing); one that explodes earlier
  is a hit (hit-stop, shake, thump).
- Effects: 0x376d0 cdecl (file, effect, pos[4], rot[4], flags, scale); bossred.edb 0x010000db has
  0x040000b7 (his flame) and 0x0400007a (his ice) - flared at the staff on each cast.
- Aim: LT/WHITE hidden from the game while Red (no mode 32). Over-the-shoulder view blended in at
  the camera hook (after modern_camera); pulled in by the game camera's own distance (its wall
  avoidance) since there is no ray cast. Left stick hidden, strafing added to item +0xc8 (divided
  by g_move_scale * g_move_xz, swept by the collision hook). Modern camera takes g_cam_want_req.
- Turn assist: rctx +0x944 points BACK toward the camera; forward = -it. Measured: the old code
  wanted pi off for forward/back pushes (sideways was right) - that was the jank. On at 10/s.
- Combo camera jump: the charge tap's camera looks well ahead of him, so size_camera's "camera on
  the player" test (target within 6) failed and his camera scaling dropped out every other frame;
  now 12.

## Test 27: element spells, charged casts, ground pound, tail-spin, hover-casting
- cast_now (redcast.c): fire = 3 shots +-0.11 rad, ice = 1 shot x0.7 speed with impact x1.6,
  electric = x1.3 speed; on an electric hit (state change with life left) two forks +-0.75 rad
  are queued and cast next tick from the hit point (not inside the projectile update), life 0.3,
  gen 1 (no further forks; quiet on timeout). Water untouched (its slot 1 doesn't use 0xd3cc0).
- Impact scale: 0x376d0 arg 11 is the effect scale; multiplied while a "big" projectile's update
  runs (g_impact_scale_pub) - charged shots 1.6 + 1.6 x charge, ice 1.6.
- Charged: while aiming, X held past 0.32 s charges 0..1 over 1 s; release > 0.2 casts three
  parallel shots (0.7 apart) at x(1.2 + 0.5 c) speed. Staff drawn back via negative recoil.
- Ground pound: B (raw, input_sdl g_raw_b_pub; B hidden from the game while pounding/spinning)
  when up for > 0.15 s. "Up" includes mode 28 and |vs| > 1.5: mode 28 is an air state (double
  jump / flight) the old air test missed. vy 3 for 0.16 s then -60 (the game caps the fall near
  -23); landed when the measured descent stops (vv > -3) for 0.04 s. Ring strikes 10 x 9.0 at
  0.8 high + 6 x 5.0 at 2.2; Spyro's impact effect 0x04000068 (file 0x01000040) at scale 3.
- Tail-spin: X with raw B held; yaw + 4 pi eased over 0.55 s, two rings of 8 (reach 8.5) at
  0.12 / 0.34 s; pose layer g_pose_spin / g_pose_dive in redpose.c modify().
- Hover-casting: aiming while up clamps phys +0x7c >= -2 for 3 s.
- Tests: XBOX_BREATH_AT=+frame:type, XBOX_POUND_LOG=1.

## Test 28: combo camera, magic meter, flight, fly-by fire, wing shield, launch, cutscenes
- Combo camera jitter: the B "charge tap" given to the game on each swing toggled charge mode 29,
  whose camera jumps 6-17 units. Removed (XBOX_MELEE_CHARGE=1 restores); redcast_camera also
  eases per-camera offsets from the player while g_cam_steady (combo, spin, pound, hit-stop).
- Flight (debugmenu.c): A held after the double jump, up = air || mode 28; stick -> turn (yaw +2.1/s)
  and pitch (vy 1.2 - 15 pitch); forward speed ours (21 +16 dive / -7 climb) through item +0xc8
  (divided by g_move_scale*g_move_xz). XBOX_FLY_LOG. Verified: turning, diving, climbing.
- Wing shield: RT hidden from the game (input_sdl g_raw_rt_pub). Block = restore hp via
  FN_SETHEALTH when it drops while shielding (the game's own hurt reaction still runs underneath).
- Projectile item hits: 0xd3d10 first calls vt+0x12c (world sweep; flag 4 = explode, no item test),
  then walks [[0x446608]]: item +0x10 bit0, head animator bit0, |pos4 diff|^2 < 100 ([0x2efcac]),
  0x78320(info,item), 0x1580e0 precise, then proj vt+0x1e8. Never reached in tests (no target in
  reach); the Red boss item has +0x10 bit0 clear most of the fight. Village items at y -3.6
  (vt 0x2f1e50, 13 of them) are not sheep. Test hooks: XBOX_GOTO_NEAR=+frame:n (+XBOX_GOTO_BRING,
  XBOX_GOTO_ALL, XBOX_GOTO_VT), XBOX_HIT_PROBE, XBOX_MELEE_VIS; XBOX_NEAR_BOSS now faces the boss.
- Crash fix: redchar_tick read g_redchar_anim (freed with the old player item) before noticing the
  item changed; guest_ptr_ok only range-checks. kmem_committed(page) is the real test.
- Cutscenes (redcut.c): XSEItemHandler_CutScene vt 0x316690 (desc 0x316664, update slot 16
  0xadbf0, messages slot 5 0xadfd0). +0x408 scene animator, +0x44c bit0 finished; [0x446528] =
  running cutscenes. Actors: scene +0x104 entries 0x20 each (count byte +0x118), entry+8 = actor
  animator (vt 0x33a3f8, recreated each shot); Spyro's = out [q+0x104] res [out+0x18] in file
  0x01000086 spyronpc.edb (48 bones). res->file: table [0x34eca4] {hash,res}, count [0x34eca0].
  Root = record 0 of [out+0x28] (0x48 each: +0x18 pos, +0x28 quat xyzw, world space). Player item
  is never animated, only locked (msg 0x1600003d). Red: hide actor bit0, move player there each
  frame, Red scale 0.35, restore position afterwards unless the game moved him. Trigger for tests:
  XBOX_WARP=+90:01000087 + LY+ from +400 (Gnasty intro c24_r1z 0x01000102, rel ~445..3100).
  New-game intro cs_r1a01 and lab c06_r1a run before a player exists.
- Movies: still no .xmv files here.

## Test 29: closer aim camera, slower flight, flight chase camera
- Aim view (redcast.c aim_view): pivot 4.0 x size up, back AIM_BACK 5.5 x size (was 10.5), side 2.1.
- Flight: cruise 9 (+6 dive / -3 climb), turn 1.7 rad/s, vy 0.8 - 8 x pitch.
- Flight camera (debugmenu.c size_camera): eased in by g_fly_cam_w; eye 5.5 x size behind,
  2.3 x size up (more when climbing), target 2 ahead / 2.4 up, smoothed. Applied only to a camera
  whose own target is within 25 of the player: the game runs a second camera (e.g. 1050e768,
  looking elsewhere) through the same apply hook, and overriding it too made the views fight.

## Test 30: flight-end camera, shield wings, squash freeze
- Camera jump after flight: g_cam_lift (0.95 x height above the take-off point, debugmenu.c) kept
  growing under the flight camera and applied at once on release. Now zero while g_red_flycam,
  with y0 reset to the current height; g_red_flycam = flying, or airborne after a flight, so the
  chase camera holds until landing.
- Shield wings: Y rotation of bones 15/21 (and tips 17/23) was the wrong way round; now
  bone 15 +1.6 / 21 -1.6 about Y (XBOX_SHIELD_WING=y,z,tip, defaults -1.6,-0.25,-0.7 as applied with
  the code's own minus signs).
- Squash: redpose captures the final pose once g_pose_squash > 0.98 and replays it (idle, breathing,
  look kept moving underneath); the squash shiver only plays on the way down.

## XMV movies (spyro-port-xmv-1, built on test 30)
- Game movie code (Eurocom wrapper, untouched): open 0x2323c0(name, audio stream) -> CreateDecoderForFile,
  GetVideoDescriptor (w/h must be 8..1024), two YUY2 textures (XGSetTextureHeader fmt 0x24, pitch w*2,
  mem w*h*4+0x80) at 0x4881c8.., GetSurfaceLevel -> surfaces 0x4881d8/dc; EnableAudioStream(dec,s,0,0,0)
  + GetAudioStream(dec,s,&0x4881f0) (AddRef'd IDirectSoundStream, game sets volume from [0x4256bc]).
  Per frame 0x232580(x,y,w,h floats from [0x4703e8]+0x18..): GetNextFrame(dec, surf[0x4881e0], &res, 0),
  res 1 = new frame (flip, 0x4881e4 = shown), 2 = EOF, 3 = fail -> close 0x232310. Draw 0x232260 -> 0x232e80.
  Decoder ptr 0x4881e8; "playing" test 0x232240. After each new frame the game runs wbinvd (0x23256c,
  0x2325c2, privileged -> SIGSEGV): nopped.
- XMV library entry points (section XMV, all stdcall) replaced by loader/xmv.c: CreateDecoderForFile
  0x2bed4d (flags, name, &dec), CloseDecoder 0x2bf291, GetVideoDescriptor 0x2bf375 (w,h,fps,nAudio),
  EnableAudioStream 0x2bf3e2 (dec, s, flags, mixbins, &stream; flags/mixbins 0 + 5.1 track -> 3 tracks),
  GetAudioStream 0x2bf561 (dec, s, &stream, AddRefs), GetNextFrame 0x2bf580 (dec, surface, &res, &rt).
  Signature-checked; mismatch -> movies hidden as before.
- Decoding: static 32-bit ffmpeg 6.1.1 (tools/build_ffmpeg.sh -> loader/third_party/ffmpeg32; xmv demuxer,
  wmv2, adpcm_ima_wav (Xbox ADPCM 0x69), pcm, wmav1/2, swscale, swresample). Frames sws_scale'd to YUY2
  straight into the surface + gl_texture_dirty. tex.c now decodes YUY2/UYVY (0x24/0x25, linear, BT.601).
  Audio: per-track ring (stereo s16 at device rate) mixed in audio_sdl.c callback (xmv_mix); 5.1 ADPCM
  downmixed. Clock = wall time from the first frame; late frames dropped. IDirectSoundStream_SetVolume
  HLE forwards to xmv_stream_volume.
- Movies requested: d:\NVUGlogo.xmv and d:\eurocom.xmv probed at the root first, then played from
  \Spyro\Binary\_bin_xb\{eurocom,NVUGlogo}.xmv at boot; NSpyro.xmv = attract movie after ~45 s idle on the
  title (skippable with A/START). fs index finds them anywhere in the game folder.
- Test files: tools/mkxmv.py makes XMVs (WMV2 + Xbox ADPCM or PCM) from any video (--test = colour bars + tone).
  Verified headless: logos 180/180 frames at 30 fps, colours right, 440 Hz tone in the SDL disk output,
  NSpyro skipped by A. Env: XBOX_MOVIES=0 off, XBOX_XMV_SKIP=1 (end every movie at once), XBOX_XMV_LOG=1.

## Test 31: movies merged, Red's shrink bolt (loader/redshrink.c)
- Built from test 30 + spyro-port-xmv-1/xmv.patch (applies clean; tools/*.sh needs chmod +x after
  unpacking the patch or make fails with "Permission denied" on build_ffmpeg.sh).
- Items: active list [[0x446608]] (node +4 next, +8 item) holds only what the game is running near
  the player. Item +0x144 = its handler (handler +4 = item). Class descriptor = target of the
  handler vtable's slot 0 ("mov eax, desc; ret"): desc +0 parent desc, +4 name pointer. Full tree:
  scan .text for b8 xx xx xx xx c3 with desc+4 -> identifier (735 classes).
- Enemies = handler descends from EnemyBase 0x2f2fa4 (fodder, MerGnorcs, imps, archers...),
  XSEItemHandler_Grunt 0x30f88c (Gnorcs, CrocoClub, crabs, critters, bots) or ChaseEnemyBase
  0x32b1ac (egg / gem thieves). Bosses are XSEItemHandler_Boss (not matched).
- Scale: item +0xe8/+0xec/+0xf0 (written each frame, the game's own changes tracked like
  scale_fields) + item +0x7e = 0; enemies don't rewrite it.
- Hit: handler vtable slot 49 (+0xc4) = OnHit(info) (base 0x24de0 = no-op; fodder 0x1739a0, grunt
  0x87fd0, critters 0x1672b0, MerGnorc 0x45720). info +0x18 push vec, +0x40 hit kind flags.
  Fodder: any hit -> death clip 0x9000054, state +0x3bc 0x18 -> 0x1a (wool off, runs about).
  Grunt: needs kind & 0x8ff and passes 0x6f890 (mask +0x3d4 & flags must have a 0xf000 "who" bit
  and another bit); health float +0x59c (-1 per hit), grace timer +0x3a8. We set health 1, clear
  the grace, flags = lowest who bit | (mask & 0x11 or lowest 0x8ff bit). Slot 69 (0x76c30) = "killed
  by these flags?" per player character type (Spyro: & 0x11); slot 70 (0x7a630) only flags
  "dying" (+0x260 |= 8) - not a kill on its own.
- Red's hidden staff shots do reach the projectile item test (side-on, not straight down - downward
  ones hit the ground first) and the test returns 1, but they never made a sheep react.
- Bolt: ours (overlay glow), 45 u/s x size, life 1.2 s, no world collision; homes in on the
  auto-picked target (dot > 0.72 with facing, nearer preferred, <= 45 away). Hit radius 1.6 + 1.4 x
  enemy scale around feet + 1.2 x scale. Shrunk: x0.3 for 10 s, grows back x6/s. Stomp: Red within
  1.3 x size + 1.6 x k of it, -0.8..3 above, falling (> 2/s) or pounding -> flat 0.12, bounce
  (phys +0x7c = 14), OnHit after 0.2 s; let go after 5 s (or pops back up small at 1.6 s if it
  has no hit handler / can't be hurt). Shrunk enemies near: lost health put back (debugmenu.c,
  next to the shield's block).
- Tests: XBOX_SHRINK_AT=+frame,.. (cast), XBOX_GOTO_ENEMY=+frame,.. (10 from the nearest enemy,
  facing it), XBOX_GOTO_ENEMY_SIDE (+XBOX_SIDE_OFS, default 5: off to one side, in view),
  XBOX_GOTO_CLASS=substring, XBOX_STOMP_AT=+frame (drop on the nearest shrunk one from 4 up),
  XBOX_ITEM_DUMP=from:every (+XBOX_ITEM_DUMP_ALL), XBOX_SHRINK_LOG. Enemies near the start:
  Dragon Shores 0100003a (CrocoClub ~20 away), Sunken City 0100005a (critters, MerGnorcSentry).

## Test 32: the staff combo made by hand (redpose.c combo_pose, debugmenu.c newc)
- Square now plays four procedural swings instead of the recorded boss clips (XBOX_OLD_COMBO=1 = old):
  A forehand diagonal cut, B backhand rising sweep, C one full spin with the staff held out (item yaw
  +-2pi over 0.06-0.34 s, direction = staff side), D leaping two-handed overhead slam.
- Each move = 5 keys (rest/anticipation/strike/overshoot/recovery) over 19 controls (sckey: TW LEAN BEND
  DROP, staff arm SP SY SO SE, off arm OP OY OO OE, staff direction AZ EL, HD head, WG wings, STEP KNEE,
  IK off hand on the shaft). Catmull-Rom through the keys, per-span ease (E_OUT into a coil, E_IN into a
  hit). Values are for the staff in the +x hand, mirrored by S = +-1. Arms are rotated before the torso
  so the twist carries them. Weight fades out from cancel+0.06 to the move's end.
- Staff direction: after staff_follow, the staff is turned about the hand from its clip orientation
  (captured before staff_follow, upright) to AZ/EL, then slerped by weight. Turning from the arm-carried
  orientation flipped through 180 deg on big swings; from the clip's own it never does.
- ik2(): two-bone reach (shoulder, elbow, wrist = a, a+1, a+2) used for the off hand on the shaft (38% of
  the way grip -> crystal) in the slam.
- Springs (stiffness/damping): tail yaw follows -twist (180/11), wings follow WG (260/14), lean (200/13).
- Timings (g_scm): hit / cancel / len / lunge window+speed / trail window per move. A press after 0.04 s
  is queued and the next move starts at the cancel point; a press within 0.3 s after a move ends goes on
  to the next; D wraps to A. Lunge speeds 15/13/6/12 u/s (34 was far too much: he walked into things).
- Hits: A/B red_melee_strike(step 0/1); C red_ring_strike(8, 8.5, 1.2) at 0.2 s; D melee step 2 (launch)
  + ring(10, 7.0, 0.8) + shake 1.1 + the pound's dust effect 3.5 ahead.
- Slash trail (debugmenu.c slash_draw): world samples of the blade (grip+15% .. crystal+22%) per frame for
  0.26 s, Catmull-Rom x4 in world space, projected, drawn as an additive ribbon (gfx/overlay.c ov_tri +
  ov_additive): element-coloured glow fading toward the grip, white-hot band on the outer 38%, a flare at
  the crystal. The old ember dots stay for the dash.
- Test hooks: XBOX_SC_POSE=m:t holds move m at time t; XBOX_SC_POSE=gN = gallery (all 36 poses, 1 s each,
  after N s, labelled on screen). XBOX_TESTCAM=dist,height,angle fixes the camera on him where
  redcast_camera runs (not in the Dragon Shores fairy cave). Copying redclips.bin into a fresh
  XBOX_SAVE_ROOT skips the 50 s clip recording.

## Test 35-anim1: Red's walk/run and jump redone (side build of test 35; redpose.c, debugmenu.c)
- Walk/run (redpose.c gait2, replaces gait; XBOX_OLD_GAIT=1 = old): same phase input (debugmenu.c, by
  distance; +x leg plants at ph = pi/2). Per leg: stance (duty 0.62 walk -> 0.36 run) sweeps the hip +A -> -A
  linearly (no skating), knee gives sin(pi u), heel peel / toe push in the last 38%; swing folds the knee
  (0.75..1.3), flicks then lifts the toes, straightens to reach. Pelvis: walk high over the foot / run low in
  the stance (q = step phase), shifted and rolled over the planted foot, yaw with the stride; chest counter-
  twists, head takes out yaw/roll/bob; run pitch 0.10. Arms counter-swing 0.35 rad late, staff arm x0.55,
  elbows with speed. Springs (dt-based): bounce -> wings and tail, hip yaw -> tail; tail raised on 41-43.
- Signs (model space, rot_world_axis about +x): + on a tail bone = DOWN (a point at +z goes to -y), - = up
  (the "tail up" comments in the dive/squash code have it the other way); + on the ankle 39/52 = toes up;
  - on knee 38/51 = fold.
- debugmenu.c: stopping advances the phase to the next k pi (feet together) at 7 rad/s while gait fades
  (rate 6 instead of 10); turning on the spot (hs < 2, |yaw rate| > 0.8) adds small steps (amount 0.07 x
  rate, max 0.28, phase += 1.6 x |rate| dt); lean += sprung acceleration term (0.012 x dhs/dt, -0.16..0.14).
- Jump: a plain jump (md 8 take-off) sets g_pose_hop = 1 (redpose.c air_pose: legs drive, toes pointed,
  chest/chin up, one wing beat, fades at 2.2/s) and ls stays LS_NONE, so the made air poses (rise/fall)
  show. The take-off clip 1ba ends in level flight, which looked like a dive on every hop. LS_AIR (flight
  loop 1b9) only when flying, gliding (air > 0.15 s; the glide flag is briefly set on the take-off frame),
  falling faster than 16/s or up more than 2.6 s. Double jump still uses 1ba. XBOX_OLD_AIR=1 = old.
- Idle gestures include his wing flourishes (19f/1b3/1f2): a test that stands still > 7 s before a jump
  sees a horizontal flight-like pose that is the gesture, not the jump.
- Test hooks: XBOX_FAKE_HS="from:to:speed,..." (treadmill: the pose layer gets that ground speed while he
  stands), XBOX_GAIT_SLOWMO=k (phase advance x k), XBOX_TESTCAM=d,h,a,1 (4th value: camera turns with him)
  + XBOX_TESTCAM_FROM=frame (game camera before), XBOX_INPUT_SCRIPT LXh+/LXh- (half stick), XBOX_GAIT_LOG,
  XBOX_LS_LOG. Recipe: XBOX_TESTCAM=5,0.6,1.5708,1 XBOX_TESTCAM_FROM=380 XBOX_INPUT_SCRIPT=+250:LY+:110
  (walks out of the start hut into the open) XBOX_FAKE_HS=400:540:5,540:640:13. Headless runs ~10 fps at
  XBOX_SCALE=1, so use SLOWMO 0.2 to see the cycle.

## Coop1: two players, vertical split screen (side build of test 35-anim1; loader/coop.c)
- Player 2 is a second player object made with the game's own spawn: spawn 0x28e60 thiscall(item mgr
  0x4464e0, class, 0x42000001, pos*, rot*, 0). Classes by type (factory 0xc2ee0, jump table 0xc311c):
  1 Spyro 0x41000041, 2 Hunter 0x42, 3 Sparx 0x48, 4 Blink 0x87, 5 Byrd 0x91, 6 Ball 0x54, 7 Ember 0xec,
  8 Flame 0xed. The spawn's init writes the player globals ([0x44c704] handler, [0x44c708] item), so
  coop.c reads player 2's from them and puts player 1's back. Destroy = 0x2d770 thiscall(handler).
- Player 2 gets its own copy of its vtable (g_p2_vt): slot 16 (update) swaps in player 2's handler/item and
  pad port byte [0x44cc90] (and the camera, below), calls the real update, swaps back. Slot 18 (deinit)
  hides Sparx [0x44c70c] (the deinit destroys him) and points the globals at itself while it runs.
  Player 1's vtable is wrapped too (g_p1_vt, re-wrapped whenever player 1's handler changes) so its
  update sees player 1's camera fields.
- Files: GetPlayersFileList 0xc1a70 cdecl(type 1..8; other types hang), file manager 0x4375a0, Load
  0x1f860 thiscall(list, mode 1 load / 2 unload / 3 IsLoaded). IsLoaded asserts (jmp $ at 0x1f980) on a
  file loaded without a job (level-owned), so readiness is checked on the resource instead: 0x1c3fa0(file)
  -> entry (every file has one), +0x24 non-zero = data loaded. Player 2 loads only files whose +0x24 is 0
  and remembers them (g_p2_own). The deinit's unload 0xc4510 is replaced (signature-checked) by
  pl_unload_files: a leaving player unloads only files the other player's list doesn't have (player 2
  only its own). Without this, either player's change unloaded the other's files (freeze a few seconds
  later, or the 0x1f980 assert). All lists share 0x1000012/0x1000040; types 1, 7, 8 share list 0x349894.
- Input: input_sdl.c opens a second SDL controller (g_gc2, hot-plug, promoted if pad 1 goes) and writes
  player 2 to g_pads[port] where port = 1 unless player 1 uses port 1. g_pads_connected = 3 makes the
  game's own manager [0x46ff98] poll it; player code reads buttons at mgr+4+port*4, sticks at
  mgr+0x84+(port*16+i)*4. Keyboard goes to player 2 when player 1 has the only pad. Back is taken off
  (g_coop_p2_back = rejoin) and the right stick goes to coop.c's camera (g_coop_p2_rx/ry).
- Cameras: player 2's follow camera is ours (p2_camera: yaw/pitch, swings behind while moving, distance
  7.5, target +1.4, no wall test). Frames alternate between views (g_coop_view): on a player-2 frame
  size_camera -> coop_camera gives any camera aimed within 30 units of player 1 player 2's eye/target. The
  render context [0x4703f0] +0x934 pos, +0x944 fwd, +0x8f4 view-proj are saved per view after present
  and swapped in for each player's update (culling/LOD and steering use the right camera; camera
  controller [[0x42bb18]+0x4b8] +0x284 eye / +0x294 target swapped too). XBOX_COOP_CAMSWAP=bits.
- Split: gl.c doubles clip-space x (c96..99 column [0], perspective draws to the back buffer only) so each
  frame is drawn at half-width aspect, blits the back buffer to view FBO [g_coop_view] at present and
  draws view 0 left / view 1 right with a small gap. coop_frame_done flips the view.
- Scan point 1 of the item manager (3 points, stride 0x18: flags, pos, zone u16 @+0x14) follows player 2
  with player 1's flags and zone, so objects near player 2 also wake.
- Health 0x43cd14 is global, so it is shared; player hit vtable 0x1e8 = 0xca670 works on player 2.
- Test hooks: XBOX_COOP_AT=+frame (spawn delay), XBOX_COOP_CHAR_AT=+f:type,..., XBOX_COOP_HURT=+f,...,
  XBOX_COOP_KILL=+f, XBOX_INPUT_SCRIPT2 (player 2's pad script), XBOX_COOP_LOG, XBOX_COOP_NOSPLIT.
  gl.c XBOX_FAST_BOOT=1 skips GL draw calls until play begins (headless boots ~2x faster). Recipe: empty
  save root, XBOX_AUTOSTART=1 XBOX_FAST_BOOT=1 XBOX_COOP=1 XBOX_COOP_CHAR=4 -> play at ~frame 9000.
- Verified headless: all 8 types as player 2, Red as player 1 with Flame as player 2, hurt/kill (life lost,
  respawn), warp, repeated Back rejoins, both players switching characters back and forth (330 s).
- Not done: Red as player 2 (his code is global for player 1), per-half HUD, camera wall test, Sparx for
  player 2, player 2 pause, scan point uses player 1's zone, Red overlays not split-aware.

## Coop2: both views every frame, the game's camera for player 2, own Sparx, AppImage (coop.c)
- Frame: game object update = vt 0x6c (called at 0x1b85fa), draw = vt 0x70 = 0x1c0da0: rctx vt 0x24
  (begin), 0x1c17f0(rctx) (the scene; the camera apply 0x1e0460 happens inside it), rctx vt 0x28;
  then present = rctx vt 0x2c (0x1cff20 -> Swap). 0x1c0da0 is replaced (byte-checked) by game_draw:
  in split screen it draws twice (g_coop_view 0 then 1) and gl_coop_capture(v) copies the back
  buffer into view FBO v after each; gl_present just composes. Afterwards rctx +0x934/+0x944/+0x8f4
  are player 1's again and the render cameras player 2's draw retargeted get their eye/target back.
  XBOX_COOP_ALT=1 = the old alternating frames.
- Steering: the player code adds the camera controller's yaw (+0x234) to the stick angle (0xc24a6,
  ~40 more reads in the player classes), so player 2's update gets player 2's camera yaw there. It
  had player 1's: reversed controls whenever the two cameras faced different ways. +0x234 = yaw of
  (look-at - eye) via atan2(dx, dz) (measured: eye->target atan2 + pi).
- Player 2's camera = the game's follow camera run twice. Camera handler [[0x42bb18]+0x4b8]: vtable
  0x3202a0, size 0x3f4 (create 0x141020, init slot 14 0x141090, update slot 16 0x144940), base
  0x14c840 over item handler 0x2d590; its own state +0x230..+0x3f4 (+0x234 yaw, +0x284 eye, +0x294
  look-at, +0x254/+0x2a4/+0x2b4 more positions, +0x384 followed item, +0x388 flags, +0x3dc mode).
  p2_game_camera (start of game_draw): swap in player 2's copy of that state (+0x384 = player 2's
  item; the first copy is player 1's shifted by p2 - p1), run the update with player 2's globals
  and pad port, keep the result, put player 1's back. The update also writes the render cameras
  (EXCamera +0x10..+0x60), saved and restored. XBOX_COOP_OWNCAM=1 = coop1's own follow camera.
- Crash (a player's log, 0x1ccc82 in 0xe0e05): FN_DESTROY defers the delete to the next update, through
  the object's vtable; the old player 2 still pointed at our single vtable copy, refilled for the new
  character, so it was destructed with the new class's destructor. Vtable copies (g_vtc[12], role
  1/2/3) are now used in turn and found by address. Player 1's change had the same problem.
- Files: the file manager keeps a reference count per load job (job +0x18): Load (0x1f080) adds one,
  Unload (0x1f120) drops one (asserts below 0), a job at 0 is purged later (the resource's +0x24 keeps
  the old pointer until then). Player 2 takes one reference on its whole list before spawning and
  its own deinit (0xc4510) drops it; coop1's unload hook and file bookkeeping are gone.
- Sparx: the player init (0xde9de) makes Sparx (0x2d4b0, class 0x4100000e) only when [0x44c70c] = 0,
  otherwise moves the existing one to itself (0x9d830). p2_spawn clears it around the spawn, so
  player 2 gets its own; his update/deinit (vt 0x314cb8 slots 16/18) are wrapped to run with
  player 2 as the player and himself in [0x44c70c]; player 2's deinit destroys him.
- Test hooks: XBOX_COOP_STACK=+frame (game return addresses at swap / camera / p2 update),
  XBOX_COOP_LISTS, XBOX_COOP_CAMDUMP, XBOX_COOP_LOG (now also rc forward vectors and camera yaws).
- AppImage: tools/mkappimage.sh (appimagetool continuous, --appimage-extract-and-run): run.sh as
  AppRun (APPDIR/APPIMAGE/OWD aware: binaries from $APPDIR/usr/lib32 with LD_LIBRARY_PATH, game
  looked for next to the .AppImage and in the start folder, log next to it), xboxrun, default.xbsym,
  and the 32-bit SDL2 with its linked libraries (not glibc, libGL/EGL, libdrm).
- Verified headless: split shots of both views each frame, steering in both axes against each camera,
  player 2's game camera at a wall, two Sparx, the crash sequence (kill, then switches through 5
  characters, player 1 switches incl. Red), Red as player 1 with Flame as player 2, the AppImage.

## Coop3: Red for player 2, and both players as Red (redctx.c, coop.c, debugmenu.c, red*.c)

Red's code was written for one player: everything it knows about "the player" lives in globals
and function-local statics across debugmenu.c (body, moves, flight, combo, size), redpose.c (pose
layer, springs, staff), redcast.c (aim, casts, magic meter), redshrink.c (bolt cooldown, pose),
input_sdl.c (his button handling). Rather than threading a per-player struct through ~4000 lines:

- Every per-player variable is marked `RS` (xbox.h: `__attribute__((section("redst")))`), so the
  linker gathers them in one section with `__start_redst` / `__stop_redst` (about 16 KB). Marked
  with tools/rsmark.py (FILE FROM TO [--apply] [--skip names]: lists, or marks, the file-scope and
  function-static variables in those lines; const data is left alone).
- Left shared on purpose: hook install flags and saved originals (`done`, `installed`,
  g_sparx_orig, redcast's class registries g_vts / g_hvts), world lists (g_rp projectiles,
  g_launch, g_forks, redshrink's g_items / g_sh / g_bolt / g_burst), the recorded clips and the
  recording (g_bc / g_nbc, g_bake_*, g_bake_done, g_baking, bake_tick's statics), settings (g_size,
  g_hide_player, g_size_rate, g_redchar_base), g_shake, g_melee_hit, boss state.
- redctx.c: the section is player 1's; player 2's copy is a heap buffer, swapped in by
  `red_ctx_set(1)` (three memcpys) and out by `red_ctx_set(0)`. Player 2's copy starts from a
  snapshot taken by a constructor before main (every variable's initial value), and is reset on
  each spawn of player 2.
- Where player 2's state is live (always together with p2_swap_in: player 2 as "the player"):
  - the item update 0x2b4d0 (thiscall item: handler update via 0x2d810, animators 0x1e6950 - where
    Red's pose is made -, handler slot 4) for player 2's item and their Sparx's item: hooked at
    its entry (bytes 56 8b f1 f6 46 10 02 74), trampoline re-runs the first 7 bytes and returns
    to 0x2b4d7 with push/ret (flags kept for its je);
  - coop_red_tick (from debugmenu_game_tick, after player 1's redchar_tick / size_tick) runs
    dm_red_player_tick = redchar_tick + size_tick for player 2;
  - input_sdl coop_pad: the Red pad handling (now red_pad()) on player 2's pad;
  - the camera hook: coop_cam_begin (replaces coop_camera) puts player 2's eye/look-at into the
    view camera on player 2's draw and makes player 2 live for the rest of size_camera (size,
    Red's flight and aim views; the modern camera stays player 1's);
    the camera's near plane is saved and put back with eye/look-at now;
  - debugmenu_draw's Red overlays (effects, magic meter, reticle): in split screen drawn twice,
    each with its half as g_pres_rect and its own view-projection (saved per view after each
    draw in game_draw, x column doubled as coop_apply does).
- Things that were run once per frame from per-player code: red_launch_tick (now once per frame
  number); g_shrink_pose's decay moved into redshrink_red_tick; redchar_animate reads its
  player's pad (g_pads[red_pad_slot()]).
- The recording of his clips (first run only): whichever player's Red starts it owns it
  (bake_tick owner, g_bake_owner checked in redpose bake_capture); the other waits.
- Player 2 type 9 = Red: spawned as type 1 (Spyro, g_redchar_base), g_redchar_want = 1 set in
  player 2's state; the menu's "Player 2" offers Red; XBOX_COOP_CHAR=9; portcfg coop_char2=9.

Tested headless (empty save + redclips.bin): player 2 Red with player 1 Spyro, both Red (walk,
jump/double jump, staff combo, aim with reticle on player 2's half, shrink bolt from each, ground
pound), and player 2 killed then switched 1 -> 9 -> 4 -> 9 -> 2 with player 1 switching to Red and
back: no faults. First run with no redclips.bin: player 2's Red recorded the clips, both used them.
Known: the stomp on shrunk enemies is player 1's only; a staff hit's
hit-stop goes to whichever Red's tick sees it first.

## Coop4: up to four players (coop.c rewritten around a player table; redctx.c, input_sdl.c, gfx/gl.c, debugmenu.c)

- coop.c: `struct cp P[4]` (index = player - 1; [0] only keeps player 1's view fields). Everything
  that was "player 2" is per player: handler/item/Sparx, character and file reference, the copy of
  the follow camera's state (+0x230..+0x3f4) and its eye/look-at/yaw, the render context's view
  fields and view-projection after their draw.
- sw_in(k, &save, cam) / sw_out(&save) make player k "the player" (PLAYER_OBJ/ITEM, SPARX_OBJ, pad
  port byte, Red state via red_ctx_set(k), and with cam=1 the rctx view fields and the camera
  controller's eye/look-at/yaw that the player code steers by). The save is the caller's, so
  nesting just works. Used by: the vtable-copy update/deinit wrappers (px_*, sx_* for Sparx; owner
  found by handle), the item update hook 0x2b4d0, p_game_camera, coop_red_tick, coop_cam_begin/end,
  coop_overlay_begin/end.
- Pad ports: player 1 keeps theirs ([0x44cc90] at rest); players 2..4 take the other ports in order
  (coop_port_of). input_sdl.c: controllers g_gcs[0..3] in connection order (a removed one moves the
  later players' pads up); player 2 falls back to the keyboard while player 1 has the only
  controller; hle's connected mask = the ports in use. XBOX_INPUT_SCRIPT3/4 for tests.
- Waking the world: the range test 0x290e0 walks the item manager's 3 scan points (0x4464e0, 0x18
  each; entry 1 is the spare one player 2 uses). For players 3 and 4 the test is pointed at our own
  table of 5 (imm32 of `mov edi,0x4464e0` at 0x290eb, `cmp ebp,3` imm8 at 0x29158 -> 5), which
  copies the game's 3 entries at each tick; entries 3 and 4 are players 3 and 4 (flags and zone
  copied from entry 0, as for player 2). XBOX_COOP_NOSCAN5=1 leaves the game's table alone.
- Views: g_coop_split = number of view slots (2 = left/right, 4 = quarters, also for 3 players; an
  empty quarter stays black). game_draw draws once per present player; gl.c keeps 4 view FBOs and
  composes halves or quarters (top row: 1, 2; bottom: 3, 4). coop_apply doubles x only for halves;
  a quarter has the screen's shape. Overlays per view with its quarter as g_pres_rect.
- Red: redctx.c keeps 4 copies; reset per player at spawn; red_pad_slot = that player's port.
- Menu: "Co-op" Left/Right/A: off / 2 / 3 / 4 players; "Player 2", "Player 3", "Player 4" pick
  characters (Red included). portcfg: coop, coop_players, coop_char2..4. Env: XBOX_COOP_PLAYERS,
  XBOX_COOP_CHAR (player 2), XBOX_COOP_CHAR3, XBOX_COOP_CHAR4; tests XBOX_COOP_PLAYERS_AT=+f:n.
- Dropped the research modes XBOX_COOP_OWNCAM (own follow camera) and XBOX_COOP_ALT (views on
  alternate frames).
- Tested headless: 4 players (Red, Hunter, Ember / Ember, Red, Red), 3 players, 3 -> 4 -> 2 -> 4,
  a lost life (all extra players go with the level reload and come back with their Sparx).
  Seen 3 times early on, then not in 7 identical runs: with 4 players player 2 walked through the
  floor of the hub's lower ground at ~5 fps headless (each frame ~0.2 s); taken to be a low frame
  rate effect, not confirmed.

## Coop5: stress tests, crash fixes, views drawn at their size (coop.c, debugmenu.c, main.c, gfx/gl.c, gfx/psh.c)
- Crash on any level change with a Ball Gadget player next to a Red player (or as Red entering a
  Ball Gadget run, single player too): g_size_cur / g_size_mult were shared, so Red's 1.3 made
  size_tick run for the Ball, whose physics (another class) has a pointer at +0x160 where the
  players' physics has the shape centre; the "+0.1 skin" arithmetic bent it and the level unload
  freed it twice (0x1d87a6, [edx] = 0x51515151, from the physics dtor 0x11c210 via item dtor
  0x2b3c5 / player handler dtor 0xc8460). Both are RS now, and the shape is never scaled when the
  player type (+0x548) is 6.
- Sparx's mini-game (mr*_spx) crashed on entry in single player: the D3D code at 0x22dda4 runs
  wbinvd (privileged -> SIGSEGV in user mode). main.c's fault handler skips wbinvd/invd (0f 09 /
  0f 08) in game code, once logged as "cpu: skipping".
- The map being played: [[0x43a9e8 +0x78] +0x88] = its Map object (debugmenu.c dm_map_class ->
  class name). Mini-games are Map_MiniGame_*. Players 2-4 sit out mini-games and Ball Gadget runs
  (player 1's type 6): no spawn while there, back next to player 1 afterwards. Tried extra Ball
  Gadgets: they spawn, but the start pad (Camera_OnBallGadgetPad + its Yes/No) launches only "the
  player", the others drop off the pillar into the sky, and teleporting them behind player 1 did
  not get them rolling on the track.
- g_cams_seen (render cameras, EXCamera vtable pointer 0x33993c) kept pointers across levels and
  p_game_camera wrote 0x50 bytes back into each: freed memory after a level change. Cleared when
  player 1 changes, and each checked for the vtable before use.
- Another camera in charge (not the follow camera 0x3202a0, e.g. a cut-scene): extra players' views
  copy player 1's view offset (p_follow_p1_view, cam_ok = 2); the camera-controller fields are
  then left alone in sw_in.
- Views drawn at their size: gl_set_view_fraction(fx, fy) makes every render target bound from
  then on draw in its lower-left fx x fy part (viewport, scissor, pixel scale; the fraction is kept
  per target, and sampling a render target multiplies its coordinates by it - psh.c kind 1 now also
  goes through texScale). coop.c game_draw sets 0.5 x 1 (halves) or 0.5 x 0.5 (quarters) for the
  views and 1 x 1 after; gl_coop_capture keeps the used part (uw x uh), gl_present blits that.
  4 players at XBOX_SCALE=1 under llvmpipe: 2.7 -> 4.0 fps. XBOX_COOP_FULLRES=1 = old way.
- Test hooks: XBOX_WARP=frame:hash:start:sub (a map object by sub id, e.g. Cloud City's Ball Gadget
  run 0100005b:-1:0500000b, Volcano Descent's 0100005f:-1:0500000c), XBOX_WARP2 (a second warp),
  XBOX_PROBE_MAP=1 (map-manager fields holding a Map). coop: XBOX_COOP_LOG also logs each new
  render camera and, at a player's deinit, its physics pointers.
- Stress runs (headless, all without a fault): 4 players with Red + Ball Gadget + Ember through
  level changes; Cloud City and Volcano Descent Ball Gadget runs in and out; Sparx, Blink and
  Sgt. Byrd mini-games in and out; Gnasty Gnorc, Red's Chamber (player 1 Red), Sunken City, Ice
  Citadel, Professor's Lab with 4 players; kills, hurts, character switches (incl. Ball Gadget and
  Sgt. Byrd for player 2) and player counts changing mid-level.
