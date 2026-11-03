# Rendering notes

GX features the game needs and how the port's Aurora implements them. Aurora changes live in
`patches/aurora/*.patch`.

<!-- Sections below: flashlight cone (color texgen) and Z textures. Other agents add their own
     sections; keep each feature in its own top-level section. -->

## Flashlight cone: color texgen (GX_TG_SRTG) and missing vertex colors

**Symptom.** In gameplay the flashlight lit the walls but its visible beam did not draw. The
intro cutscene uses a different model (`opwf_cone`, drawn by the game's S3D code) and was fine.

**The draw.** Gameplay uses the J3D model `handlight_ef.bmd` with the texture-scroll animation
`handlight_ef.btk`, both from the static archive (`kt_static/`), loaded by `fn_800AA128`
(names at `0x80359750`/`0x80359764`). Its three materials (read from the BMD's MAT3 section):

| Material | Stages | Texgens | Notes |
|----------|--------|---------|-------|
| `hl_mat_hlc_a` (core) | 3 | TEX0 x TEXMTX0 (2x4, btk scroll); POS x TEXMTX1 (3x4, view projection map); **SRTG from COLOR0** | stage 0 color = lerp(C0, C1, TEXC) with C0 = (255,255,100) yellow; alpha = TEXA x TEXA x TEXA(SRTG) |
| `hl_mat_hlin` | 4 | same plus TEX1 x TEXMTX2 | |
| `hl_mat_hlout` | 4 | same | |

All three: one color channel, COLOR0 lit by light 7 only (mask 0x80) with clamp diffuse and
spot attenuation (the flashlight light), additive blend (SRCALPHA, ONE), Z compare LEQUAL
without update, Z compare before texturing, no alpha test, no fog, no indirect, K colors white.
The core material takes its material color from the vertex (`matSrc` = vertex) although the
model has no vertex colors.

Two Aurora gaps hid it:

1. **Color texgen used the wrong source.** `GX_TG_SRTG` (XF texgen types 2/3) sets s,t to the
   R,G of the *lit* color channel result (COLOR0 or COLOR1). Aurora read the raw vertex color
   attribute instead, which this model does not have, so every lookup hit texel (0,0), the
   alpha that multiplies the beam was 0 and the additive blend added nothing. Now the texgen
   reads the channel's lighting output (`out.cc0`/`cc1` in the vertex shader), the color
   channel is computed even when no TEV stage uses RASC/RASA, and, like on hardware, the
   texture matrix, post-transform matrix and normalization are not applied (they only apply to
   regular texgens). The beam's brightness now follows the spot light's falloff.
2. **Missing vertex color = black.** With `matSrc` = vertex and no color in the vertex, Aurora
   used (0,0,0,0), so the core material's lit color was 0 and the core vanished. On hardware
   the color of the last vertex that had one lingers (see Dolphin issue 13783); that history
   is not tracked, so the port now uses white, which is what Dolphin does. Checked which draws
   this changes: from boot through the foyer only the flashlight core and one 5-stage material
   whose missing COLOR1 RGB is never read by its TEV, so nothing else changes.

Before/after (`LM_TEST_STATE=poltergust LM_START_MAP=2 LM_START_APPEAR=1`, the nursery, Luigi
turned to face the camera with scripted stick-down taps): before, only the bulb glows; after,
the beam fans out with its yellow core, and from the side it is the familiar soft cone.

Cost: none (shader generation only).

## Z textures (GXSetZTexture)

`GXSetZTexture(op, fmt, bias)` writes BP `0xF4` (24-bit bias) and `0xF5` (format bits 0-1:
0 = Z8, 1 = Z16, 2 = Z24X8; op bits 2-3: 0 disable, 1 add, 2 replace). With an op set, the
texel fetched by the **last TEV stage** (whether or not the TEV uses its color) is read as an
unsigned depth value, the bias is added, and the sum either replaces the rasterized depth or
is added to it, wrapping at 24 bits. Depth testing then happens after texturing.

Implementation (Aurora patch `0008-color-texgen-and-z-textures.patch`):
- `GXSetZTexture` emits the two BP registers; `regs.cpp` decodes them (so display lists that
  write them directly work too) into `GXState::zTexOp/zTexFmt/zTexBias`.
- The pipeline config carries the op and format when the last stage has a texture and
  texcoord; the shader then samples that texture and writes `@builtin(frag_depth)`.
  The bias is a uniform (changing it does not create pipelines).
- Texel interpretation follows the hardware texture unit, which only sees the low 4 format
  bits: Z8 data is read as I8 (z = I), Z16 as IA8 (z = A << 8 | I, A being the first,
  high-order byte), Z24X8 as RGBA8 (z = R << 16 | G << 8 | B). The texture-object load maps the
  Z formats to those formats, so the data decodes like any other texture.
- The 24-bit result is converted to Aurora's depth convention (reversed Z:
  `1 - z / 0xFFFFFF`); for ADD the rasterized depth is first converted to the 24-bit value.

**Uses in Luigi's Mansion.** `LM_TRACE=801F828C` (the game's `GXSetZTexture`) and the
disassembly show one user: `fn_8000B228` (European `fn_8000B7CC`), the sub-viewport clear of
the Game Boy Horror screen (view 1). It draws a black quad with `GXSetZMode(on, ALWAYS, on)`
and `GXSetZTexture(REPLACE, Z24X8, 0)` from an all-0xFFFFFF texture, resetting that rectangle
to the far plane, then disables the Z texture (`GXInit` also disables it). The port used to
move the quad onto the far plane in a hook (`HLE(fn_8000B228)`/`palfn_8000B7CC`); that
workaround is removed, and the live GBH camera (`LM_GBH_CAM=2`, foyer) renders through the
real Z texture path.

Cost: only draws with a Z texture get the extra sample and depth write (one quad per frame
while the GBH camera is on), so there is no Rendering toggle.

Not covered: Z textures built from EFB depth copies would depend on the copy's encoding.
Aurora's Z16 copy conversion stores the high byte in I and the low byte in A (the opposite of
the IA8 view the hardware gives a Z16 texture), so a game sampling such a copy as a Z texture
or as IA8 would read swapped bytes. Luigi's Mansion never feeds a copy to `GXSetZTexture`, but
it does make EFB depth copies: `fn_8005D8CC` copies Z16 (640x480, with clear) and the code
around `0x8005EA5C` copies Z8. How those textures are sampled (and so whether the Z16 byte
order matters) has not been checked yet.

*Update (EFB copies to RAM, below):* the Z16 copy is the game's shadow map (section "Shadows"), read only as an
IA8 texture by a TEV `GR16` compare. The game's own depth-ramp LUT and swap tables make that
compare consistent only if the IA8 view of the copy has I = depth bits 23..16 and A = bits
15..8, i.e. the console stores a Z16 copy low byte first (the reverse of an ordinary big-endian
16-bit texel). That is what Aurora's conversion and the copy-to-RAM encoder produce. A Z16
*Z texture* built from such a copy would then presumably compose z from I (high) and A (low);
nothing in LM does that.

## Rooms vanishing behind doorways (foyer entry flicker)

**Symptom.** In the cutscene when Luigi first enters the foyer, on the close-up of Luigi with
the staircase behind him, the upper wall and glass ceiling above the stairs disappear (black)
for about 8 frames, then pop back (the user's screenshots showed one frame with them and one
without). Same at Original 30 and at 4:3, so not interpolation or widescreen.

**Not a renderer bug.** A per-frame display-list log (`LM_DL_TRACE=<file>`, below) shows the
game itself stops calling `GXCallDisplayList` for every part of two models (the room behind
the stairs, `0x80EF3D00..0x80EF7D40`, and its furniture, `0x8078A180..0x8078D980`) in exactly
the frames where the block is missing. Aurora's vertex-array sizing/sharing (patch 0002),
`GX_CULL_ALL` (0004), draw merging, scissor and viewport were therefore not involved.

**How the game picks rooms.** `fn_80012EE0` walks the portal graph each frame: for every
doorway of a visible room, `fn_8001333C(portal, outRect, parentRect)` gets the doorway's four
corners (`fn_80018238`), multiplies them by the camera's projection*view matrix
(`lbl_803A3C98 + 0x258`, built by `fn_80014228`), divides by w, maps to the screen rectangle
at `lbl_803A3C98 + 0..6`, takes the bounding box (one pixel margin) and intersects it with the
parent rectangle (`fn_80013200`). A non-empty rectangle adds the room behind the doorway to
the draw list with that scissor (`fn_800130BC` -> `fn_8001792C`; drawn by `fn_8001798C`).

**The bug (original game code).** There is no near-plane clipping. The only check is that
not all four corners are behind the camera. With the camera close to the doorway into the
room behind the stairs, two corners have w < 0; dividing by a negative w mirrors them, all four
projected corners land right of the screen (x = 750..1300) and the intersection with the
parent is empty, so the whole room is skipped. Logged in the bad frames: corners
`(985,211) (967,-45) (751,456) (1191,-175)`, 1/w = `0.001, -0.003, -0.002, 0.002`.
The recompiled code computes what the console would (`fctiwz` saturation and the 16-bit
stores match, and the inputs are plain game math), so this most likely happens on hardware
too; it is fixed as an enhancement.

**Fix.** `src/recomp/portal_fix.cpp` wraps `fn_8001333C` (hook `us pal`; the PAL function
differs only in relocations). After the original runs, if the doorway straddles the near plane
(some corners with z + w < 0 in GX clip space, but not all), it replaces the rectangle with the
box of the doorway clipped to z + w >= 0 (corners in front plus the plane crossings of every
corner pair), mapped and intersected with the parent the same way. Doorways fully in front or
fully behind keep the game's result, and a closed door (`fn_8001B278`) stays empty. Setting:
F1 > Enhancements > General > "Fix rooms vanishing near doorways" (`portal_clip_fix`, on by
default); `LM_PORTAL_FIX=0/1` overrides it for A/B tests. Cost: negligible (only doorways that
straddle the camera are recomputed).

Verified: the whole foyer-entry cutscene captured frame by frame (no missing room in any
frame; with `LM_PORTAL_FIX=0` the 8-frame gap returns), and title, file select, a training
map, foyer gameplay and the Nursery look the same with the fix on and off. Patch 0008 (color
texgen / missing vertex color) does not affect it: with 0008 merged and `LM_PORTAL_FIX=0` the
room still drops out for the same 8 frames (the newly visible flashlight beam on that shot
makes the black block easy to miss). The other display-list gaps left in that cutscene are
frames where the clipped doorway is genuinely off screen.

**Widescreen / field of view.** The same wrapper also puts the portal rectangles in the Hor+
frame (docs/ENHANCEMENTS.md, "Scenes fixed for widescreen"): rooms are drawn with the
widened projection, but the rectangles came from the 4:3 projection*view matrix and were then
used unchanged as the room's `GXSetScissor` (stretched to the window). Rooms seen through a
doorway were cut off at a scissor edge shifted outwards from the doorway, which showed as a
black slab next to pillars in the portrait gallery (map 6), and rooms visible only in the
widened sides were culled. While `fn_8001333C` runs, rows 0 and 1 of the matrix at
`lbl_803A3C98 + 0x258` are scaled like the projection (`ws::factor() * fovRatio` and
`fovRatio`) and restored afterwards; nothing else reads that matrix. This part is not tied to
the portal setting.

**Debug aid.** `LM_DL_TRACE=<file>` writes one line per frame (`F<n>`) listing every
`GXCallDisplayList` as `addr:size@caller<caller...` (guest stack back chain). Diffing the sets
of consecutive frames finds draws that drop out for a few frames. `LM_DL_TRACE_FRAMES=<first>,<last>`
limits the trace to a window of its own frame numbers.

**Side observation (not the cause, not changed).** Patch 0002 shares dynamically sized array
uploads per frame by pointer (`sDynamicUploads`); `GXInvalidateVtxCache` clears each array's
`cachedRange` but not that map, so an array the CPU rewrites in place mid-frame would reuse the
frame's earlier upload. Nothing in this cutscene does that.

## EFB copies to RAM

Toggle: **EFB copies to RAM (accurate)** (`[rendering] efb_copy_to_ram`, default on;
`LM_EFB_RAM=0/1` overrides). Code: Aurora patch `patches/aurora/0009-efb-copy-to-ram.patch`
(`lib/gfx/efb_ram.{hpp,cpp}`, C API `include/aurora/efb_ram.h`) and `src/recomp/efb_copy.cpp`.

### Console behaviour

`GXCopyTex` writes the EFB rectangle into main memory as a texture in the format given to
`GXSetTexCopyDst` (tiled: 4 bpp in 8x8 blocks, 8 bpp in 8x4, 16 bpp in 4x4, RGBA8/Z24X8 as
4x4 blocks of 64 bytes with the AR pairs followed by the GB pairs; big-endian texels). Anything
that later reads that memory, a texture of any format/size/offset, the CPU, or DMA, sees those
bytes; anything that writes it replaces them.

### What Aurora did

Copies stay GPU-only: the copy is resolved into a texture (at the upscaled render resolution)
keyed by the destination pointer, and any texture whose data pointer equals that key samples it,
whatever format and size it was created with. Memory is never written, and the association lives
until the pointer is copied to again, even after the game frees the buffer and loads something
else there.

### What Luigi's Mansion does with its copies (US, measured with `LM_COPY_LOG=1`)

| Destination | Copy | Read as | Code |
|---|---|---|---|
| shadow buffer (heap, 0x96000) | Z16 640x480, every frame | **IA8 640x480** texture (TEV `GR16` compare against a 256x256 IA8 depth ramp: the flashlight/lightning shadow map, see "Shadows") | `fn_8005D8CC` copy, `fn_8005D41C` texture, `fn_8005DD50` TEV setup |
| `lbl_804D86BC` (0x8021E000) | RGB565 640x480, twice per frame | RGB565 same size | `fn_8005F518` / `fn_8005F8B8` |
| `lbl_804D86B8` (0x802B4000) | RGBA8 320x240 (box-filtered) | RGBA8 same size | `fn_8005F498` |
| 0x803A4D00 | RGB565, box-filtered half size of a screen region (70x100 from (250,140 140x200) in the foyer, 134x114 from (186,126 268x228) in the 2F Bathroom) | same | `fn_80025C98` |
| transition captures | RGB565 640x480 | same | `fn_800081A8` (LMDisplayUtil) |
| title/file select | 4 x RGB565 148x116 | same | `fn_800494E4` |
| event 5/6 render targets (heap, +0x28 of the objects at `r13+0x768`/`+0x76C`) | IA4 (event 5) / RGB5A3 (event 6) 128x128, every frame while the event runs | same, warped by the indirect stage (Mario painting; see "Indirect effects") | `fn_8006033C` |

- The shadow buffer is the one copy read with a different format. It and the two screen
  buffers are also reused as scratch memory for ARAM/DVD loads (Game Boy Horror archive,
  `fn_80121884`; `fn_80066DB8`, `fn_8005336C`), i.e. written by DMA/CPU between copies.
- After the screen-buffer copies the game calls `GXPixModeSync` + `DCInvalidateRange` on them
  (cache hygiene before reusing the memory, no CPU reads of the pixels were found). No GXPeek
  calls exist.
- The 148x116 title buffers are freed after the menu and the memory is reused for other textures
  (e.g. Luigi's IA8 textures at +0x540/+0xD60) at different pointers.

### What the port does now

1. **Every copy** of a supported format (all of Aurora's copy conversions plus RGBA8/RGB5A3) is
   recorded as a memory range `[dest, dest + GXGetTexBufferSize)` with a hash of the memory
   there (`efb_ram::on_copy`, FIFO thread).
2. **Textures** (`resolve_sampled_textures`): a texture overlapping a recorded copy
   - exactly matching it (pointer, format, size) keeps Aurora's fast copy texture (upscaled:
     the enhancement on top of the accurate base);
   - otherwise (different format, size or offset, or spanning several copies) is **decoded from
     the copy's GX bytes**: a compute pass encodes the copy texture into the exact console bytes
     (native resolution, tiling, byte order: `EncodeShader`), and a fragment pass decodes them
     as the requested format (`DecodeShader`, same conventions as Aurora's CPU decoders). Both
     run before the render pass that samples the texture, in the same frame, with no CPU sync.
     Partial overlaps start from the memory under the texture and lay each copy's bytes over it
     in copy order. Palette (C4/C8/C14X2) and CMPR reads keep Aurora's existing paths.
   - The shadow map is the case that takes this path in LM: the Z16 copy is read as IA8 (unless
     "Shadow map at render resolution" is on, see "Shadows").
3. **CPU/DMA writes**: when a texture is resolved over a recorded copy, the memory is re-hashed
   (once per frame per copy); if it changed, the copy is dropped and Aurora's pointer association
   evicted, so the texture comes from memory. (Fixes stale copies shown for reused heap memory
   with the same pointer.)
4. **CPU reads**: copies into ranges the game `DCInvalidateRange`s (how GameCube code makes
   GPU-written memory visible to the CPU) are encoded and read back into guest memory. The
   readback is asynchronous: the bytes are written at the next guest frame boundary after the GPU
   finishes the frame (1-2 frames after the copy), and only if the memory is unchanged since the
   game *issued* the copy (or still holds our previous writeback), so a CPU write after the copy is
   never clobbered (see "Late copy readbacks" below: Aurora 0222).
   A same-frame synchronous readback would need a mid-frame GPU submit/stall, which Aurora's
   single-encoder frame model doesn't support; LM never reads copy pixels with the CPU, so this
   is not needed here. `LM_EFB_READBACK_ALL=1` reads back every copy (test aid).

Encoded copy semantics follow Aurora's copy conversions (`tex_copy_conv.cpp`): e.g. Z16 = depth
bits 23..16 in the IA8 intensity byte and 15..8 in the alpha byte, which is how LM's shadow TEV
(swap tables + `GR16` compare) expects it.

### Results

- Bit-exactness check: in the intro (copies on vs off, 2x internal resolution, 550 frames dumped
  at native resolution) the frames are identical, i.e. the GPU encode/decode of the shadow map
  gives the same image as Aurora's direct Z16 sampling there. (Feeding the decode deliberately
  shifted bytes also left the intro door scene unchanged: the self-shadow receiver doesn't affect
  those shots.) The mouth flicker itself is rare and random and was not reproduced; this feature
  removes the copy-related differences from hardware (native-resolution, byte-exact reinterpretation
  of the shadow map, stale-copy eviction). Re-check if the flicker is still reported.
- Regression run (title, file select, intro, foyer with HUD, Game Boy Horror map screen) renders
  correctly with the toggle on.

### Cost (foyer gameplay, 2x internal resolution, Original 30)

- CPU in efb_ram (hashing, resolves, writebacks): ~0.6 ms per guest frame (~5 MB hashed per
  frame: 5 copies + writeback checks), on the FIFO/host threads. Whole-process CPU over 25 s:
  33.6 / 33.2 % of a core on vs 30.4 / 33.1 % off (within noise).
- GPU: one compute encode + one fullscreen decode of the 640x480 shadow map per frame, plus
  encodes of the two DCInvalidateRange'd screen buffers (~1.5 MB/frame read back). Negligible next
  to the scene.

### Test aids

- `LM_COPY_LOG=1`: every GXCopyTex (frame, dest, rects, format, caller LR), textures created
  over copied memory (same/different format), and efb_ram counters every 300 frames.
- `LM_FRAME_DUMP=<dir>,<first>,<last>[,<step>]`: writes guest frames `<first>..<last>` as
  native-resolution `.ppm` files, captured through the copy-to-RAM path (works with the toggle
  off too). Frame numbers count GXCopyDisp calls; runs drift a few frames apart, so align by
  content when comparing.
- `LM_EFB_RAM=0/1`, `LM_EFB_READBACK_ALL=1`.

### Lock order: the Sealed Room / mirror-warp freeze (Aurora patch 0191)

Reported in the first full playthrough (2026-10-12): the game froze ("not responding", Windows
closed it: three `AppHang` events in the Application log, no crash dump) while loading the Sealed
Room after dropping down the roof chimney, and when using a mirror to warp back to the foyer.

Cause, from the all-thread stacks of a frozen run (`LM_HANG_DUMP=8`): a lock-order deadlock
between efb_ram's mutex and Dawn's device lock (on since `ImplicitDeviceSynchronization`, Aurora
0175, 2026-10-11).

- Render worker: `Queue::Submit` (frame submit, `aurora.cpp`) holds the device lock; Dawn fires
  the `AllowSpontaneous` MapAsync callback of an EFB-copy readback inside it, and the callback
  locked `g_mutex` to queue the landed bytes.
- GX FIFO thread: `resolve_texture` held `g_mutex` and called `new_conv_texture` (Dawn
  `CreateTexture` → device lock) for a texture decoded from EFB copy memory for the first time.
- Guest audio thread (JAudio `DspbufProcess` → `DCInvalidateRange` → `aurora_efb_ram_cpu_read`)
  then waited for `g_mutex` while holding the guest CPU, so no guest thread ran again and the main
  loop waited for a frame forever.

Readbacks complete every frame (the two `DCInvalidateRange`'d screen buffers), so the callback side
is always there; the trigger is the first decode of a new texture over copy memory, which special
transitions do (the Sealed Room arrival decodes an 84x110 RGB565 texture over a 70x100 copy) and
normal doors rarely do. Any Dawn object created or released under `g_mutex` (dropped copies,
evicted copy textures, old decoded textures) was the same hazard.

Fix (0191): the map callback takes only leaf locks (`g_landedMutex` for the landed list, the
readback pool lock, whose new buffers are created after unlocking); `g_mutex` is never taken in a
callback, and nothing is created or released under it (the decoded texture is created between two
locked passes; dropped entries/textures and copy-texture evictions happen after unlocking). A
guest thread in `DCInvalidateRange` therefore no longer waits behind the device lock (e.g. a
pipeline compile) either. The comment at `g_mutex` in `lib/gfx/efb_ram.cpp` states the rule.

Evidence: with a 400 ms sleep held under `g_mutex` at the first decode (temporary stress hook) and
`LM_EFB_READBACK_ALL=1`, the unfixed build froze on the Sealed Room arrival with exactly the three
stacks above; the fixed build ran on in the room (3 of 3 runs, 400/800 ms). The unstressed freeze
was also seen once in about a dozen unfixed runs (same audio-thread stack in `LM_WATCHDOG`).
Workaround for older builds: F1 > Visuals > Rendering, "EFB copies to RAM" off.

Same pattern elsewhere: upstream `depth_peek.cpp` (GXPeekZ) creates buffers under its mutex
(`ensure_slot`) and its spontaneous map callback takes that mutex; LM never peeks Z, but a port
that does (SMS?) has the same deadlock.

Regression test (US save with the roof open; `LM_CARD_DIR` scratch copy):
`LM_START_APPEAR=3 LM_SKIP_TEST=1450:pos:2402,2300,-2566 LM_HANG_DUMP=8 LM_INPUT_CLOCK=frames
--input "480:START*10,880:A,1000:A,1100:A,1200:A,1300:A,1400:A"` drops Luigi into the roof
chimney (the real route: Sealed Room event, Luigi at 1910,550,-2770, room 37); mirror warps:
stand in front of a mirror and press A (furniture `move` 22 in map2 jmp/furnitureinfo: foyer
(420,0,-400), jmp rooms 17 (-1960,280,-4380), 38 Wardrobe Room (-1540,780,-3860), 42
(-1960,830,-4380), 36 Sealed Room (3765,860,-2790)). Sweep on the fixed build (teleport in front,
A in four facings, `LM_HANG_DUMP=8`): the foyer mirror warps (twice in a row, no stall); in the
other four rooms nothing froze, but no warp happened either: with the test save they are still
dark (ghosts appear), and a mirror only warps from a lit room. Re-test those with a save that has
them cleared.

### No Dawn calls in map callbacks: the render-loop freeze (Aurora patch 0219)

Seen in long frame-interpolation runs (Bogmire's arena, after 2.5-5 min; also Original 30): the
picture stopped for good while the process lived. `LM_HANG_DUMP=6` stacks:

- main thread: `aurora::gfx::acquire_frame_slot` -> `wait_for_gpu_progress` (forever);
- render worker: inside Dawn (from the frame's end_frame lambda), waiting on a Dawn condition variable;
- present-timing waiter (`present_timing::waiter`, Aurora 0197/0216, always on: pacing.cpp enables it):
  `Instance::WaitAny(future, 0)` -> Dawn fires the EFB readback's `AllowSpontaneous` map callback ->
  the callback's `Unmap` / `GetConstMappedRange` -> Dawn `Mtx_lock` (device lock), blocked;
- game thread: `VISetNextFrameBuffer` (frame::present), waiting for the host.

The waiter polls every ~0.25 ms, so spontaneous callbacks often run on it; a callback that calls back
into Dawn while another thread holds the device lock and waits for that event deadlocks inside Dawn.
Rule: **a spontaneous callback makes no Dawn call** (it records a status and queues work, leaf locks
only). 0219: the readback callback queues the readback in `g_mapped`; `efb_ram::finish_maps` copies
the bytes out and unmaps, holding no lock, on the render worker after each frame (`gfx::after_submit`)
and in `aurora_efb_ram_apply`. The readback pool stores each buffer's size (no `GetSize` under the
pool lock). Same scenario on the fix: 15 min, and Bogmire -> death -> game over -> title twice
(10 min each, judder log on/off): no stall. (efb_blend6's `AURORA_EFB_BLEND6_TIMING` callback and
upstream depth_peek still call Dawn in callbacks; both off in LM.)

### Late copy readbacks: memory corruption under a busy CPU (Aurora patch 0222)

User crash (2026-10-14, new game, lab/training and early mansion, 120 fps interpolation, Enhanced
lighting Ultra, two other games/builds holding the CPU at 100%, game at 21-26 fps): after ~16 min
`[recomp FATAL] call to 814D6E20: no function starts there (lr=8019C598)`. `lr` is in
`Jac_UpdateDAC` (JAudio's AI-interrupt work, audio thread): it calls `DAC_CALLBACK_FUNC`
(`.sbss` 0x804D9324, never set by the game, always null) and that word held 0x814D6E20. For the 1.8 s
before, the main thread read ~12,300 addresses 0xCC3E493E..0xCC3F698E six bytes apart (a vertex-like
walk through a pointer that held 0xCC3E49xx). Two unrelated places held junk 16-bit pairs at once:
game memory had been overwritten, the audio path was only where it surfaced.

Cause: the EFB-copy readback check. The game's 640x480 Z16 shadow-map buffer (0x96000 bytes, heap) is
copied into every frame and, like the two screen buffers, reused as scratch memory for ARAM/DVD loads
(the Game Boy Horror archive, see above); once the CPU `DCInvalidateRange`s part of it (the 0x162E0
range in the user's log: "CPU reads EFB copy memory at ... (+0x162e0)"), every shadow copy is read
back. A readback landed whenever the memory still matched what Aurora saw when it *executed* the copy.
With frame interpolation the copies run at the frame's first replay on the render thread while the
game is already ahead (more so when the host threads are starved); if the game had meanwhile loaded
something into the buffer or freed it to the next map's heap, Aurora hashed those new contents as the
"pre-copy" memory, the readback matched, and 600 KB of old depth values were written over the new
data (the junk pairs above are plausible depth/pixel values). In lockstep (Original 30) Aurora runs
the copies while the game waits, so the window was only the readback's own 1-2 frames.

Fix (0222): `aurora_efb_ram_guest_copy`, called from the `GXCopyTex` HLE on the game thread before the
copy is queued (`efbcopy::guestCopy`), records the range's hash as the game left it; the readback's
pre-copy hash is that one (Aurora's own hash stays the fallback for copies the game did not announce,
e.g. frame dumps). A writeback lands only if the game has not written the range since issuing the copy
(or it still holds our previous writeback): the state a console would be in, since its copy lands at
once and later writes stay. Steady-state readbacks are unchanged (each lands over our previous one).

Evidence (`LM_DEV_WARPS` cycling maps 1/3/2 every 600-700 frames, `LM_TEST_STATE=poltergust`, the
game pinned to 4 E-cores shared with 4 busy loops, 20 min): with the old check (temporary switch) 14
late readbacks were written over memory the game had changed after issuing the copy (40 warps), e.g.
the old map's shadow copy over 0x80FB09E0 right after the warp to map 3; with the fix the same
situations are logged and dropped, no crash (20 min / 40 warps, and 21 min / 50 warps: 20+ drops
each; `late EFB copy readback ... dropped`, first 20 shown;
`[efb] ... (late N)` in `LM_COPY_LOG` stats). The exact user crash needs a particular heap layout and
was not reproduced; starved runs without warps (foyer, training room, 15-30 min each) never hit the
window at all, and none of them crashed.

Things ruled out on the way (docs/AUDIO.md pacing, starved runs with `LM_IRQ_STALL`, `LM_AUDIO_LOG`,
`LM_WATCH=804D9324`): AI interrupt give-ups (up to 175 in 30 min) and JAudio's DSP overload check
(22,600 forced voice stops in 15 min with a test stall in `DSPReleaseHalt`) degrade the sound but
never wrote guest memory out of place; no guest store or DVD/ARQ write ever touched 0x804D9324.

## Vertex snapshots (Luigi's mouth/hose flicker)

**Cause.** The game passes its vertex arrays to `GXSetArray` as pointers into main memory, and Aurora's
FIFO thread reads them when it processes each draw. Some arrays are rewritten by the CPU while
earlier draws that read them are still queued: the vertex-morph animation (`fn_8005C760` -> 8x
`fn_8005C464`, then `DCStoreRange`) rewrites Luigi's face positions (about 0x4A4 bytes per buffer).
If the FIFO thread is still behind when that happens, the queued draw uses the new data. The
mouth is then drawn in the wrong shape for one frame. How often this happens depends on timing,
so it looks random. Dolphin's dual-core mode has the same race in the same scene (issue 12903).

**Evidence** (US, Original 30, `LM_INPUT_CLOCK=frames`, `--input "600:START*10,1000:A*6,1400:A*4"`,
intro forest walk, `LM_FRAME_DUMP` frames 1000-1450):
- `LM_VTX_RACE=1` counts guest stores into array ranges that a published but unprocessed draw
  reads live. With snapshots off it counts about 1.27 million such stores in intro frames
  ~1000-1500, at normal timing and with `AURORA_FIFO_DELAY_US=1500`, all from the morph code.
  With snapshots on it counts 0, in the intro, the foyer and the nursery (Poltergust, walking,
  vacuuming), with and without the delay.
- The intro is deterministic once frames are aligned by +-4. Four runs with snapshots on are
  identical around Luigi's face. With snapshots off, 2 of 4 runs had 4 and 9 single frames whose
  only difference is Luigi's mouth (a dark mouth shape over the mustache in a frame where the
  neighbours and the snapshot runs have none). The other 2 runs had none.

**Fix** (`src/recomp/interp_vtx.cpp`, `snap*`; F1 > Visuals > Rendering "Snapshot vertex data at
draw time", `[rendering] vertex_snapshot`, on by default). On the guest thread, each draw is
parsed to find which elements of each indexed array it reads. Display lists use the same parser as
the interpolation tapes, with results cached per list and vertex layout. Immediate-mode draws
emit a placeholder `GXSetArray` that is re-pointed once the indices are known. That range is copied into a
per-frame arena (3 generations, power-of-two blocks so Aurora's size-0 upload rounding stays
inside), and Aurora is pointed at the copy. Copies are shared per guest address until
`GXInvalidateVtxCache`, as tapes do, and the live arrays are rebound at the end of each frame.
Lists that cannot be parsed (recorded through Aurora) fall back to the live arrays. Above 30 fps
the interpolation tape already snapshots arrays, and this path is idle.

**Cost** (game thread, `LM_VTX_SNAP_LOG=1`): foyer 0.28 ms/frame (540 draws, 1.4 MiB copied);
nursery gameplay 0.18 ms/frame (277 draws, 0.7 MiB).

**Unchanged.** At Original 30, frames match the old build except for the glitch frames. With
enhanced lighting, the only differences between snapshots on and off are on Luigi's face along
his path. Interpolation at 120 shows the same tape and blend statistics as the old build. The
shipped exe's fresh setup reaches the foyer.

**Test aids:** `LM_VTX_SNAP=0/1` (overrides the setting), `LM_VTX_RACE=1` (race detector: hooks every guest
store, slow; add `LM_TRACE=ring` for the last called function), `LM_VTX_SNAP_LOG=1` (stats
every 300 frames), `AURORA_FIFO_DELAY_US=<n>` (Aurora 0181: the FIFO thread sleeps before each
batch to provoke the race).

## GX accuracy audit (TEV, EFB format, copy filter, fog, lighting, textures)

Aurora patch `0010-gx-hardware-accuracy.patch` (incremental on 0001..0009); host side in `src/recomp/hle_gx.cpp`
(`gx::applyAccuracySettings`, display copy state) and `src/recomp/crt.cpp` (present chain);
toggles in F1 > Visuals > Rendering. What the game uses was traced with `AURORA_GX_AUDIT=1`
(each distinct pipeline feature, logged once) and `LM_GX_AUDIT=1` (state calls with arguments)
from boot through the title, the foyer (dark, flashlight) and the lab/training room. Test aids:
`LM_ACCURACY=<mask>` (1 integer TEV, 2 EFB color depth, 4 texture copy filter) and `LM_ACCURACY_AB=<s>` (alternate
configured/all-off every s seconds and log fps per phase, for A/B screenshots of one run).

| Area | What LM uses | Aurora before | Status |
|------|--------------|---------------|--------|
| EFB pixel format | `GXSetPixelFmt(RGBA6_Z24)`, dither toggled per draw | 8-bit target, no dither | **Fixed** (toggle "EFB color depth", on): output quantized to 6 bits/channel with a 2x2 ordered dither when the draw has dither on, expanded back to 8 bits; blended draws quantize color only (alpha stays the blend factor; the blend itself is still 8-bit). Verified A/B in the lab: mean -0.9 levels; a dark area goes from ~98 to ~25 distinct levels (dither grain/banding as on hardware) |
| TEV arithmetic | ADD/SUB, unclamped regs, scale x2, GR16 compare, K0-K3 selects, swap tables, up to 8 stages, `GXSetTevColorS10` | float lerp, [-4,4] clamp, k/8 konsts | **Fixed** (toggle "Integer-accurate TEV", on): 8-bit A/B/C (low byte of a register), s11 D/registers, C widened by C>>7, per-stage rounding, clamp 0..255 / -1024..1023, konst k/8 = k*32, HALF = 128, 8-bit vertex colors, rounded indirect texels, 8-bit fog blend. Verified: lab scene changes ~50% of pixels by exactly 1 level, unbiased, no breakage. SUB/DIVIDE_2 rounding is a model (+-1 LSB) |
| Display copy | `GXSetCopyFilter(aa=0, vf=1, {6,0,20,12,20,0,6})`, gamma 1.0, clamp top+bottom, Y scale 1.0, 640x480 | GXSetCopyFilter/Gamma no-ops; YScale returned 0 | **Fixed** (toggle "Display copy filter", on): present-time pass with the game's taps (0-1 line above, 2-4 this line, 5-6 line below = 6/52/6 of 64) in EFB lines (scales with resolution), edge clamp, gamma 1.0/1.7/2.2, before the CRT filter. `GXSetDispCopyYScale` returns the XFB line count. Texture copies: see the next row |
| Texture copies (GXCopyTex) | the same taps are set while the game copies the scene (RGB565 640x480, RGBA8 320x240 box-filtered, region and transition copies) | copied without the filter | **Fixed** (patch 0018, toggle "Texture copy filter", on; see "Copy filter on texture copies") |
| Early Z + alpha test | `GXSetZCompLoc(1)` with alpha compare GREATER 0 and Z update | Z written only for passing pixels | **Verified, unchanged**: emulating "Z written before the alpha test" (depth prepass) broke Luigi's face decals and pillars, so the hardware evidently does not write Z for alpha-rejected pixels here. Experiment removed |
| Fog | only `GX_FOG_NONE` seen; range adjust enabled (center 0), `GXInitFogAdjTable` once | range factor from a guessed NDC formula (much weaker than the table) | **Fixed model**: LUT built from the table (entry i = 256x factor at 32(i+1) EFB px from center, interpolated, 1.0 at center, last entry beyond), in EFB pixels at any resolution. Not visible in tested scenes (fog off) |
| Texture filtering | min LINEAR/NEAR, edge LOD, no mips | hw min-filter decode had NEAR_MIP_LIN/LIN_MIP_NEAR swapped | **Fixed** decode table (raw-BP texture setups). LOD bias/edge LOD fine |
| Lighting | spot (clamp diffuse), specular (shininess form), dist-attn fns, SRTG from COLOR0 | per-vertex, same formulas | **Reviewed, no change** (SRTG lit-color fix is in 0008). Open: Aurora normalizes transformed normals; hardware may not |
| Indirect | Only the Mario painting at the Secret Altar (`fn_8005FDD4`, event 6) and an invisible copy of it in event 5; J3D materials on the disc have no indirect stages | float offsets | **Fixed** (patch 0014, see "Indirect effects"): integer offsets (s1.10 x s8 >> 3, scale shift, 1/128-texel fixed-point coordinates) in integer TEV mode; ITF_5/4/3 low-bit offsets and positive-modulo wraps always. Offsets are in logical texels, so they scale with resolution |
| Lines/points | width 6 (1 px), viewport jitter field=1 only | width/6, perpendicular expansion | No change needed for LM (hardware widens along x/y; noted) |
| Logic ops, dst alpha | no logic op (`LM_GX_AUDIT` boot to foyer: blend modes NONE/BLEND only; logic-mode calls log their guest call chain); dst alpha used | only CLEAR/COPY/NOOP, any other logic op was a fatal error | **Robustness** (patch 0026, from SMS 0140): all 16 ops; exact where a blend can express them (CLEAR, COPY, NOOP, SET, INV, INVCOPY via a white/inverted shader source in the free high bits of `ShaderConfig::fogType`, so existing pipeline configs and the shipped precache list stay valid), mask-exact for AND/OR/REVAND/INVAND/NOR/INVOR, XOR/EQUIV/REVOR/NAND approximated |

### Toggles (F1 > Visuals > Rendering, all on by default, persisted in settings.ini `[rendering]`)

| Toggle (ini key) | What it does | Visible effect | Cost (measured below) |
|---|---|---|---|
| Integer-accurate TEV (`integer_tev`) | TEV stages evaluated in integer WGSL (8-bit A/B/C, s11 D/registers, per-stage rounding/clamp) | +-1 level on ~40-45% of pixels, unbiased (mean +0.06); nothing structural | see table |
| EFB color depth (`efb_color_depth`) | fragment output quantized to the EFB format (RGBA6 / RGB565) with the PE's 2x2 dither when the draw enables it; texture copies to RGB565/RGB5A3 keep their format's precision (patch 0014) | ~1.4 levels darker on average (LM draws much of the scene with dither off, where the PE truncates), 6-bit banding rings in smooth gradients (flashlight pool on the floor), dither grain where dither is on | see table |
| Smooth color banding (`smooth_banding`, **off**: an enhancement, see "Smooth color banding") | EFB and RGB565/RGB5A3 copy quantizers output the local average of their steps (same mean colour), plus sub-step noise; off (not needed) while enhanced lighting turns the 6-bit EFB off | no 6/5-bit contours, blotches or dither grain; colours within ~1 level of the accurate picture | none measurable |
| Display copy filter (`display_copy_filter`) | present-time pass (`dcopy_filter` in crt.cpp, ahead of the CRT chain) with the game's GXSetCopyFilter taps + GXSetDispCopyGamma | ~5% less vertical high-frequency detail (6/52/6 of 64), no color shift (mean 0.2 levels); only runs when the taps/gamma change the picture | see table |
| Texture copy filter (`texture_copy_filter`) | GXSetCopyFilter taps applied to color GXCopyTex copies (patch 0018, `GX_AURORA_ACCURACY_COPY_FILTER`) | the scene the game redraws from its RGB565 copy is softer vertically (nursery: vertical gradient energy -30%, horizontal -6%; differences only along horizontal edges, no shift) | a copy that was a plain texture copy becomes a shader blit with three taps |

The fog range LUT and the texture min-filter decode are plain correctness fixes with no
toggle (no runtime cost: the LUT is rebuilt only when the fog range state changes; LM never
enables fog).

### Verification (merged build, US, `LM_FRAME_DUMP` at 1x internal resolution)

Deterministic runs (`LM_INPUT_CLOCK=frames`, nursery `LM_START_APPEAR=1`,
`LM_TEST_STATE=poltergust`, flashlight sweeps, X/B for the GBH) with all toggles off, TEV only,
EFB only and all on produce frame-identical scenes, so the same dump frame can be diffed:
- TEV only vs off: mean |d| 0.2; 0.3-2% of pixels differ by more than one level, at most
  3-12 levels on a handful of isolated pixels at the flashlight beam's edges; flashlight
  cone, beam core, Luigi's self-shadow (Z16 copy read as IA8, GR16 compare), GBH HUD, coins,
  bear/ball/door models identical by eye.
- EFB only vs off: mean |d| 1.4, max 8-11 levels; 93-97% of dumped pixels are
  exact 6-bit expansions (the rest come from blended draws, whose blend result is still 8-bit:
  WebGPU cannot quantize after fixed-function blending).
- Title screen, file select, lab dialogue, foyer (web, flashlight), Portrificationizer, training
  room with a gold ghost, gallery, GBH scan view (sub-viewport 3D + EFB copies), GBH map and
  inventory screens: rendered with all toggles on, no shader errors, no broken colors.

### Performance

RTX 3090 Ti, 5120x3840 internal (resolution scale 8), Original 30, nursery gameplay;
`LM_ACCURACY_AB=10` alternates each configuration with everything off and GPU utilization is
sampled with nvidia-smi (other test instances shared the GPU, so these are averages over ~5
on/off phase pairs). **Provisional: measured under load** (several other game instances and
builds were running on the machine); to re-measure on an idle machine.

| Configuration vs all off | Median paired GPU-utilization delta | Paired deltas (pts) | Frame rate on / off |
|---|---|---|---|
| Integer TEV only | -2.3 pts | -6.7, +6.7, -2.6, -2.3, +3.9 | 29.9 / 29.8 |
| EFB color depth only | -0.3 pts | +16.0, -21.7, +0.5, -14.4, -0.3 | 24.4 / 27.2 (machine load, both phases) |
| Display copy filter only | -6.3 pts | +2.0, -6.3, -8.4, -6.8, +1.6 | 29.9 / 29.9 |
| All on | +6.9 pts | +8.4, -2.4, -3.3, +6.9, +7.1 | 29.8 / 29.8 |

The spread between pairs (up to +-20 points) is far larger than any toggle's effect, so the
honest reading is: each toggle's cost is below the noise floor of this setup. Even at
5120x3840 every configuration held the game's 30 fps in both phases, and an earlier run at
2880p with uncapped interpolation stayed at 60 fps with and without the toggles. Expected
costs from the implementation: integer TEV and EFB depth are a few extra ALU ops per pixel
(no extra passes; changing either rebuilds pipelines once), the copy filter is one extra
full-screen pass with three texture taps.

Remaining: blended draws are not quantized after blending; whether hardware normalizes
transformed normals; fog range LUT with widescreen is untested (LM never enables fog).

### Copy filter on texture copies (Aurora patch 0018)

The console applies the vertical copy filter (BP 0x53/0x54, set by `GXSetCopyFilter`) to every
EFB copy, texture copies included. LM sets one filter for the whole game, `{6,0,20,12,20,0,6}`
(`LM_GX_AUDIT=1`: the only GXSetCopyFilter combination from boot to the nursery), so its scene
copy (`fn_8005F8B8`, RGB565, redrawn as the frame), the 320x240 RGBA8 copy, the half-size region
copies and the transition captures are all filtered on hardware. Ported from the SMS port (its
patch 0102), with LM specifics:

- `GXSetCopyFilter` writes BP 0x53/0x54 (pass-through taps `{0,0,21,22,21,0,0}` when vf is off),
  so the state also survives capture/replay (frame interpolation); `bp_copy_filter` keeps the
  taps in `GXState::copyVFilter`.
- `resolve_pass_into` packs the weights of the line above (taps 0+1), the line (2+3+4) and the
  line below (5+6) into `tex_copy_conv::Uniforms::filterTaps` (0 = off: pass-through taps, depth
  copies, or the toggle off). The neighbour taps are **one row of the copy source** apart: exactly
  the hardware's three lines at native resolution; above it they only soften. (Taps one EFB line
  apart, as first ported, put two sharp duplicates of the image a few rows above and below into
  every upscaled copy: "ghost copies" at 3x; SMS found it, its patch 0110.) `GXSetCopyClamp`
  (now forwarded to Aurora; BP 0x52 bits 0-1 kept in `GXState::copyClamp`) makes the source
  rectangle's top/bottom row its own neighbour. `sample_efb` applies it to RGB before the format conversion and the
  5/6/5 quantization; alpha comes from the line itself. Filtered copies that would have been a
  plain `CopyTextureToTexture` take the blit shader instead (`RenderPass::resolveFiltered`).
- Depth copies (the Z16 shadow map) are never filtered.
- Box-filtered (mip) copies sample with LINEAR at 2:1, so the centre and neighbour taps each
  average two rows: the result equals the console's order (filter each line, then average 2x2).
- `LM_FRAME_DUMP` sets pass-through taps around its dump copy (the dump stays the raw EFB) and
  restores the game's taps.
- Toggle F1 > Visuals > Rendering > "Texture copy filter" (`texture_copy_filter`, on),
  `GX_AURORA_ACCURACY_COPY_FILTER` (`LM_ACCURACY` bit 2).

Verification (US, `LM_FRAME_DUMP` with `LM_ACCURACY=3` vs `7`): title frame mean |d| 0.5
(max 4); in the nursery the static background differs only along horizontal edges (vertical
gradient energy -30%, horizontal -6%: the 6/52/6 vertical blur and less vertical dither), no
shift, no seams at copy edges. Not compared with a console capture.

Ghost-copy fix (row spacing, from SMS patch 0110 / its crt.cpp): the display copy filter
(`dcopy_filter` in crt.cpp) likewise averages the whole EFB line for the taps above and below
(the line itself stays one row), so the presented frame softens instead of showing sharp copies
one EFB line away. Measured in the nursery with window captures at 1920x1440 (3x): the
autocorrelation of the vertical gradient had spikes at 3 rows (+0.50) and 6 rows (+0.11) before
(duplicate edges from the texture copy and the display filter); after, it decays smoothly
(+0.40, +0.18, +0.12, +0.09, ~0 from 5 rows). At 1x, native frame dumps of the old and new
spacing are identical (max 0-5 levels on a few pixels where the copy clamp now applies).

## Smooth color banding (Aurora 0190, enhancement)

F1 > Visuals > Rendering > **Smooth color banding** (`[rendering] smooth_banding`, **off** by
default = the console's picture; under "EFB color depth", which it needs). Where the banding comes
from is measured in docs/ACCURACY.md section 11: mostly the RGB565 scene copy the game pastes back
every frame (5-bit red/blue: green/blue blotches and contours in dark rooms), then the RGBA6 EFB
(LM draws its models and rooms with dither off, so these are bare truncation steps).

**What it does.** The accurate pipeline stays (same copies, same passes, same alpha, same
precision decisions); only the colour *output* of each quantizer is replaced by its local average:

- EFB writes (`efb_quantize`, shader.cpp): the 6/5-bit value the PE would store is replaced by the
  widened level ramp `X(k) = (k << (8 - n)) | (k >> (2n - 8))` interpolated at `t`, where `t` is the
  mean stored level for a smooth input: `(c - (c >> n)) / step` for dithered draws (the dither adds
  0..step-1 uniformly, so its mean is exact), `(c - (step - 1) / 2) / step` for truncating ones (the
  8-bit TEV output loses 1.5 levels on average). Alpha keeps its exact 6-bit value (dst alpha /
  blend factor semantics unchanged). Selected per draw by bit 16 of the `current_pnmtx` immediate,
  so no pipeline config changes: no precache re-sweep, toggling it compiles nothing.
- Sub-step noise (bit 17, always on with the option): +-0.5 of an 8-bit step of interleaved gradient
  noise by render pixel before the RGBA8 target rounds, so very dark gradients do not show the PC's
  own 8-bit steps and the rounding does not bias the mean (without it the smoothed picture came out
  ~0.4 levels darker: the truncation ramp lands on half levels). Static pattern, no flicker.
- RGB565 / RGB5A3 texture copies (`tex_copy_conv`, `quantize = 2`): `smooth8` = the n-bit level
  ramp shifted by the average the copy drops from a 6-bit EFB value, `2 * (2^(6-n) - 1)` (2 for
  5 bits, 0 for green's 6, 6 for RGB5A3's 4).
- 6-bit blending (0176) is skipped (no resolve passes) while smoothing is on: blends stay 8-bit.
- The bytes of copies to RAM (`efb_ram` encoder) keep the exact format; only the picture changes.

The alternative of "just 8 bits" (EFB color depth off) was rejected: it is +3.1 / +1.6 / +4.5
levels brighter in R / G / B in the dark nursery (bluer, lifted blacks), i.e. not the game's
colours. A post-process deband filter was not needed: the bands are removed at their source, so
HUD and texture detail are untouched and nothing blurs.

**Enhanced lighting** (branch `enh-fixes`): the 6-bit EFB is turned off altogether while it is on
(`gx::enhancedOverridesEfbDepth()`, "Enhanced lighting fixes" below), so the lit picture is plain
8-bit and the smoothing has nothing to act on; the checkbox is greyed out with the 6-bit group and
its tooltip says why. (The first version of this option forced the smoothing on under enhanced
lighting instead; turning the 6-bit EFB off gives the enhanced gradients without the sub-step noise.)

**Verification** (native frame dumps, nursery `LM_START_MAP=2 LM_START_APPEAR=1
LM_TEST_STATE=poltergust LM_LIGHTNING=0,90,1`, flashlight + GBH; window captures at render scale 3):
- Off: byte-identical to main's build (23/23 frames 2580-2800 incl. lightning, beam, GBH). Note
  that runs fall into a few lightning-timing variants even with the same binary (main's build:
  23/23, 5/23 and 9/23 against its own reruns), so compare against a run of the same variant.
- On vs accurate, mean per channel (block averages, dark frames): R -0.5..-0.7, G +0.1..+0.3,
  B +0.9..+1.0 levels (8 bits: +3.1 / +1.6 / +4.5); by brightness the residual is -1.5..+1 below 64
  and up to +3 above 128 (where a step is a few % of the value). Distinct colours per frame
  ~4.5k -> ~40-50k; equal horizontal neighbours in smooth areas 66 % -> 30 %, full-step jumps
  28 % -> 11 %. HUD, GBH screens: within 0-3 levels.
- Render scale 3: the ceiling/wall blotches and the beam's contour rings are gone, texture detail
  and edges unchanged; enhanced lighting (forced) loses the contour lines in the beam and pool.
- 120 fps, render scale 2, `LM_INTERP_TEST=3` (tools/interp/scenarios.sh nursery, frames
  2580-2700): 0 stale pixels, snapped/unpaired counts identical to the same run with smoothing off;
  in-between frames are smoothed like the game frames (6-bit share 3 %).
- Cost: ~20 ALU ops per pixel on quantized draws, one branch on an immediate; no extra pass (and
  the 0176 resolve passes are skipped). At render scale 6, uncapped, on a shared GPU the frame-rate
  samples were dominated by other load (on 51-65 fps, off 30-43 fps across alternating runs): below
  the measurement noise.
- Test aid: `LM_SMOOTH_BANDING=0|1|2` (off / on / on without the sub-step noise) overrides the
  setting; `LM_ACCURACY=<mask>` with bits 0x20 (smooth) and 0x40 (noise).

## CRT filter: video signal paths (S-Video, composite, RF)

F1 > Visuals > CRT filter > **Signal**: RGB (clean, the old behaviour), S-Video, Composite, RF
(antenna / coax). It is a separate choice next to the tube preset rather than more presets: the
cable and the tube are independent (any tube could be fed any signal), so four signals x four
tubes would otherwise be sixteen presets, and choosing a tube preset keeps the signal (and the
other way round). Choosing a signal loads its defaults; "Signal advanced" has the sliders (luma
bandwidth = sharpness, chroma bandwidth, sharpening, colour, Y/C separation and artifact strength
for composite/RF, and for RF noise, ghost + delay, jitter, hum bar, interference) and "Reset signal".
Settings `[crt] signal, separation, luma_bandwidth, chroma_bandwidth, sharpening, artifacts,
saturation, noise, ghost, ghost_delay, jitter, hum, interference`. An original model written
from the NTSC signal definitions (no shader code from other projects).

### The model (`src/recomp/crt.cpp`, passes `ntsc_encode/filter/separate/decode`)

The passes run on the game's NTSC line, not the output: one row per scanline (480, or 240 with
"240 lines") sampled at 4 x the colour subcarrier (4fsc = 14.318 MHz). A 640-pixel game line lasts
47.4 us at the GameCube's 13.5 MHz pixel clock, 679 samples; a wider (Hor+) frame gets
proportionally more (the postfx ASPECT size mode), so the bandwidths are the same per screen width
at every aspect and the result is independent of the output resolution (the CRT passes upscale it
as before). Values stay gamma-encoded, as in the real signal.

At 4fsc the subcarrier advances exactly 90 degrees per sample, so every carrier sample is +-1 or 0
and the modulated chroma of a sample is +-I or +-Q. That makes the whole chain exact and cheap
while the textures hold Y, I, Q (the composite signal is Y + I cos + Q sin, formed on the fly):
1. `ntsc_encode`: box-average of the game frame over each sample, R'G'B' -> Y'IQ (FCC matrix).
2. `ntsc_filter`: the console's encoder band limits (luma 6 MHz, chroma 1.5 MHz, Hann-windowed
   sinc FIRs, 33 taps). RF adds the modulator + tuner/IF channel limit (4.2 MHz) applied to the
   *modulated* signal: on the I/Q pair that is the low-pass shifted to the subcarrier, a complex
   kernel (in-phase + quadrature taps), so the tuner's cut of the upper chroma sideband costs some
   saturation and smears hue as on a real set. RF also adds a multipath ghost (a delayed, weaker
   copy; its chroma arrives at another carrier phase, rotated by the delay in quarter cycles), a
   hum bar (mains 60 Hz against 59.94 Hz fields: one dark band per field rolling up every ~17 s),
   an optional interfering carrier (diagonal bars) and white noise per sample, new every field.
3. `ntsc_separate`: the TV's Y/C separation. S-Video keeps them apart. Composite/RF only have
   Y + C: a notch set takes chroma with a +-1.3 MHz band-pass and removes a narrower +-0.8 MHz
   trap from luma (luma keeps more detail; the chroma sidebands outside the trap stay in luma =
   dot crawl, luma detail near 3.58 MHz is decoded as colour = rainbows); a 2-line / 3-line comb
   subtracts lines of the same field (opposite subcarrier phase) then band-passes, so flat colour
   separates cleanly and the dots hang on horizontal colour edges instead. Linear, so Y and C go
   through separately and the crosstalk terms are scaled by "Artifact strength" (1 = the true
   composite sum). Then demodulation (x 2cos / 2sin of the burst-locked carrier).
4. `ntsc_decode`: TV luma channel (low-pass at "Luma bandwidth" + peaking "Sharpening"), chroma
   low-pass: an analog-style causal filter (three real poles at "Chroma bandwidth", zero at fs/2
   for the 2fsc demodulation product) advanced by two time constants as the TV's luma delay line
   does, so colour trails to the right of edges; colour control, RF black lift / gain (AGC: a
   slightly flat picture); Y'IQ -> R'G'B'. RF line jitter (per line per field + slow wobble) is
   applied where `crt_linearize` reads the decoded line.

Line ends read the nearest sample with the same carrier phase (no colour fringe at the borders).
Defaults: S-Video luma 5.0 / chroma 1.3 MHz; Composite 4.2 / 1.0 MHz, notch, colour 0.92,
sharpening 0.25; RF 3.4 / 0.8 MHz, colour 0.85, noise 0.2, ghost 0.08 at 1.2 us, jitter 0.2 px,
hum 0.25.

### Subcarrier phase and frame interpolation

Phase per row in quarter cycles: 227.5 cycles per line (+180 degrees per line of a field) and
262.5 lines per field (+90 degrees per field), so in the 480-line picture the rows of one field
alternate by 180 degrees, the two fields are interleaved a quarter cycle apart, the pattern moves
every field and repeats after four (two frames): dot crawl. Rows of the field not being sent keep
the phase of their last field. "240 lines" uses 263-line double strike timing (+180 degrees per
line and per field).

The field counter runs on **real time at 59.94 Hz** (steady clock, `fillSignal`), not on game frames
or presented frames. The console sends 59.94 fields per second whatever the game's frame rate
(LM renders 30 fps, each game frame goes out in two fields with different subcarrier phases), and
the TV shows each field for 1/59.94 s: at 120 fps each field is presented twice, exactly what a
set would show, and the crawl speed does not change with the frame-rate setting, interpolation,
pauses or slowdowns. Game frames would halve the crawl and freeze it in menus; presented frames
would make it twice as fast at 120 fps. The CRT's 480i scan field now uses the same clock
(before, it toggled every presented frame).

### Cost and checks

GPU (RTX 3090 Ti, `AURORA_GPU_PROFILE`, render scale 5 = 2400-line source, frozen title or foyer;
the light load keeps the clocks low, so these are upper bounds): composite with a 3-line comb
+0.27 ms per presented frame (encode 0.01-0.02, filter 0.04-0.06, separate 0.12-0.13, decode
0.07), RF +0.28-0.44 ms; the CRT passes after them are unchanged. The signal textures are
~680-910 x 480 at any output size, so the cost does not grow at 4K output.

With the CRT filter off the chain is the display-copy pass only, as before; window captures of a
frozen frame (`LM_FREEZE_AT`) from the build before and after are identical except the title's
pulsing "PRESS START" (the freeze lands on a different blink frame). ImGui ID sweep
(`LM_MENU_ID_CHECK`) of Visuals: no conflicts.

Comparison captures (title + foyer with Luigi and the HUD, aperture grille, 480 lines, 1920x1080
16:9 Hor+): `out/crt_signals/` (not in git). Test aids:
- `LM_CRT=<preset 0-3>[,<scanlines 0-2>[,<signal 0-3>]]` (signal 0 RGB, 1 S-Video, 2 composite, 3 RF);
- `LM_CRT_FIELD=<n>` holds the field clock (identical screenshots);
- `LM_CRT_CYCLE=<seconds>[,<start s>]` steps RGB -> S-Video -> composite -> RF (logs `[crt] signal`);
- `LM_FREEZE_AT=<frame>` (main.cpp) holds the game from that frame (copyDispCount) while the host
  keeps presenting its last tape through the post chain (needs a frame rate above 30): A/B shots
  of one game frame, e.g. `LM_CRT_CYCLE=5,25 LM_FREEZE_AT=550` (title) or `3400` (foyer with the
  HUD on the `600:START*10,1000:A*6` route).

## Indirect effects

Aurora patch `0014-indirect-and-copy-format-accuracy.patch`; logging and test aids in
`src/recomp/hle_gx.cpp` (`LM_IND_*`).

### What uses indirect texturing (US; the European game has the same code)

- **Static search.** In the DOL, the GX indirect calls (`GXSetIndTexOrder/CoordScale/Mtx`,
  `GXSetNumIndStages`, `GXSetTevIndirect/IndWarp`) are made only from `fn_8005FDD4`, from
  `GXInit`/`GXSetTevDirect`, and from J3D (`J3DSys::drawInit` resets, `J3DInd*::load`). None of the
  J3D models on the disc (`Effect/*.bmd` are MAT2, the rest are in `game*.szp`, `Iwamoto/vr*.szp` and
  `Map/map*.szp`) has an indirect material block, so `fn_8005FDD4` is the only indirect effect.
- **Runtime sweep** (`LM_IND_LOG=1` + `AURORA_GX_AUDIT=1`, which also reports indirect state that
  arrives as raw BP writes in display lists): boot, title, file select, foyer, every map 0-13 by warp
  (with and without entry events: Bowser, Chauncey, Boolossus, Bogmire, map 12), the mansion appear
  points 3/5/7/9 (Balcony Boos, Sealed Room, King Boo at the Secret Altar), the lab, Portrificationizer,
  the fire and ice elements with L/R held in the foyer and nursery, the PAL Hidden Mansion
  (`LM_HM_FORCE=1`: foyer, appear point 3, map 12). Indirect draws only appear in
  events 5 and 6 (below). Ghosts, Boos, the vacuum vortex, elements, water, the GBH, mirrors and
  screen transitions do not use indirect texturing (the mirrors and transitions are EFB copies, below).

### The effect: `fn_8005FDD4` (warped render-to-texture)

What starts it is not a per-map table: `0x803002C4`, `0x80300478` and `0x8030062C` are three parallel
arrays of 109 function pointers (init, per-frame update, end) indexed by **event number** (followed by
the string `/Event/event%02d.szp`). Two events use the effect:

| Event | Where | Init / end | What is rendered |
|---|---|---|---|
| 6 | Secret Altar, King Boo's talk (mansion appear point 9, trigger at 2313,-550,-6457) | `fn_8002CD48` / `fn_8002CEB0` | two S3D models (`fn_800605BC`) into two 128x128 **RGB5A3** copies, cross-faded (`r13-0x7758` and 255 minus it): **Mario in the painting** |
| 5 | 2F Bathroom (trigger at -1649,550,-4889, character `fat`, flag 38) | `fn_8002CC8C` / `fn_8002CD14` | the model of the actor of type 0x5F into one 128x128 **IA4** copy |

`event05.szp`/`event06.szp` are empty placeholder scripts; the effect comes only from these code
slots. Each frame `fn_8006033C` renders the model with its own perspective into a 128x128 viewport
and `GXCopyTex`es it to the object (+0x28, format from +0x00); `fn_80060A9C` (scene draw,
`fn_8000BB98`) then calls `fn_8005FDD4(texObj, alpha)` for each object:

- TEXMAP0 = the 128x128 copy; TEXMAP1 = a 64x64 IA4 texture at `0x803CFD40` that `fn_8005FD80`
  (from `BootScene::init`) fills with random intensities 0-15 (alpha 0);
- indirect stage 0: texcoord 1 (vertex TEX1, identity), scale 1; TEV stage 0
  `GXSetTevIndWarp(signed, no replace, ITM_0)` = ITF_8, bias STU, no wrap;
- matrix `[[f f 0] [f f 0]]`, exponent 1, with `f` = `r13+0x760` = 0.01 (s1.10: 10/1024), so
  offset_s = offset_t = 2 x 10/1024 x (I - 256) texels, -5..0 texels of the 128x128 copy;
- colour = the copy, alpha = copy alpha x `alpha`, blended over the scene; a world-space quad at
  fixed coordinates (`0x80339B80`: x 2220..2375, y -180..-375, z -7306, the painting frame at the
  Secret Altar), s 0.1..0.9, t 0..1. Event 5 draws the same quad there, so nothing of it is visible
  in the bathroom; its IA4 copy is also an extra TEV stage of the draw in `fn_80027D28`
  (`fn_80027B30`, reached from `fn_80011824`).

**Texcoord 1 is beyond the texgen count** (Aurora patch 0015). `fn_8005FDD4` sets a texgen for
texcoord 1 (vertex TEX1, identity) but never calls `GXSetNumTexGens`; the count it inherits is 1 (from
`fn_80025E9C`'s draw just before, in both versions). Aurora, like the usual emulator convention, read
such a coordinate as (0,0): every pixel read noise texel (0,0) (intensity 51 at the boot seed), so the
painting was only shifted by a uniform -4 texels and Mario looked clean.

A real-console capture of the scene (King Boo's talk, "Get me outta heeeeere!") shows Mario
scattered over the whole painting, with the grain of the 64x64 noise texture across the painting
(about 1/64 of its width per cell, white speckles in the cap emblem, a broken-up moustache) and
the displacement of a few texels. So the console produces a varying coordinate 1, not a constant.
Candidates and what they predict:

| Rule for a coordinate >= numTexGens | Painting |
|---|---|
| (0,0) / any constant (stale per-pixel value) | uniform shift, clean Mario: contradicted by the capture |
| stale rasterizer setup from an earlier primitive | depends on whatever was drawn before; would change with the camera and Mario's animation and show a seam between the quad's two triangles: not what the capture shows |
| generated from its texgen registers as if counted (TEX1 here, 0..1 over the quad) | 64 x 64 noise cells over the painting: matches |
| a copy of texcoord 0 (s 0.1..0.9) | 51 x 64 cells: also close; the capture is too soft to separate it from the previous rule |

Patch 0015 implements the third rule (`GX_AURORA_TEXGEN_BEYOND_GENERATE`, the default): a
coordinate a TEV stage samples, or an indirect stage reads, at or beyond `numTexGens` is generated
from its texgen registers when they are a matrix texgen whose source the vertex provides (TEX0-7,
POS, NRM); anything else keeps (0,0). It changes only draws that read such a coordinate:
`LM_TEXGEN_LOG=1` (host side) and Aurora's one-time "texcoord N read beyond numTexGens" log found
exactly the painting's two draws (texcoord 1, numTexGens 1, source TEX1) through boot, title, file
select, foyer, nursery with the vacuum and GBH screens, and the Secret Altar; two identical nursery
runs with the old and new rule differ no more than two runs with the same rule. The magnitude
then follows from the game's own data with no tuning: the 0.01 matrix x 2 gives 0..-5 texels of the
128x128 copy (both axes, so the smear is diagonal), and the noise texture and copy are sampled the
way the game set them up (noise point-sampled, 64x64 repeat; copy bilinear, clamp). At 4x the copy
is rendered at 512x512 and the offsets stay in its 128-texel units, so the picture is the same,
only sharper (image 7). `LM_TEXGEN_BEYOND=0|1|2` switches between (0,0), the default and
texcoord 0 for comparison (image 8).

### Hardware semantics vs Aurora (patch 0014)

| | Hardware | Aurora before | Now |
|---|---|---|---|
| Matrix | s1.10 entries (`GXSetIndTexMtx` truncates x1024), exponent 2^(n) | floats of the same entries | unchanged (exact) |
| Offset | integer `M x (s,t,u)` (s8 after bias) >> 3 = texels with 7 fractional bits, then the scale shift (right shifts floor) | float product x 2^n | integer TEV mode: the same integer steps (`floor(floor(raw/8) x 2^n)/128`); float mode unchanged |
| Base coordinate | fixed point, texel units, 7 fractional bits | float | integer TEV mode: floor to 1/128 texel; dynamic S/T matrices likewise (`uv128 x s >> 8`) |
| ITF_5/4/3 | offset = low 5/4/3 bits, high bits = bump alpha | high bits (`floor(v/8)`...) | low bits (matches Aurora's own bump-alpha masks) |
| Wrap 16..256 | masks the fixed-point coordinate (positive modulo) | WGSL `%` (negative stays negative) | `x - m floor(x/m)` |
| Units | texels of the sampled texture | texels of the game's texture object (`GXTexObj` size, not the upscaled copy) | unchanged: offsets keep their size at any internal resolution and aspect |
| Indirect texel | 8-bit components (.abg = s,t,u) | rounded in integer mode (0010) | unchanged |
| Texcoord beyond numTexGens | not documented; the console capture shows it varying (see above) | (0,0) (patch 0001) | generated from its texgen registers (patch 0015) |

Not changed (LM never uses them): `indLOD` (LOD from the unmodified coordinates), add-previous after
a direct stage (Aurora does not carry a direct stage's coordinate into `t_TexCoord`), wrap/add-prev on
stages whose indirect stage index is >= `numIndStages`.

For the painting the integer path changes each offset by at most a few 1/128 texel (e.g. -4.016
instead of -4.004 texels); no visible change. The 1x/4x and 16:9 checks below are the real verification of the scaling.

### EFB copies met along the way

- The painting copies (RGB5A3) were blitted without the format's precision; IA4 copies (event 5)
  were already quantized. Patch 0014 adds an RGB5A3 conversion (alpha >= 0xE0: RGB555 opaque,
  otherwise RGB444 + 3-bit alpha, channels expanded by bit replication) and 5/6/5 precision for
  RGB565 copies, under the "EFB color depth" toggle (`GX_AURORA_ACCURACY_EFB_DEPTH`, default on; off
  keeps 8 bits). The painting copies hold only opaque and alpha-0 texels (`LM_IND_LOG` with
  `LM_EFB_READBACK_ALL=1` counts them: about 9300 opaque, 7050 alpha 0, 0 translucent).
- RGB565 copies in LM: the full-screen scene copy that `fn_8005F8B8` redraws every frame (the PAL
  Hidden Mansion mirror flips it), the box-filtered half-size region copy at `0x803A4D00`
  (`fn_80025C98`; 268x228 -> 134x114 in the 2F Bathroom), transition captures (`fn_800081A8`) and
  the title/file-select thumbnails. Since the scene copy is redrawn, every frame now has 5-bit red
  and blue as on the console: mean |d| ~1 level per channel, at most 8-21 levels where the copy
  filter and blending meet a 5-bit step (image 6); no structural change.
- The upscaled fast path keeps copies at render resolution; a box-filtered (mip) copy averages the
  matching render pixels.

### Verification (US, `LM_FRAME_DUMP` native frames, `LM_INPUT_CLOCK=frames`)

Images in `build/indirect_fx/` (not in git):
1. `1_painting_warp_off_on_diff.png`: `LM_IND_OFF=1` / default / |difference| x4, same frame of
   two deterministic runs; only the painting differs (Mario 4 texels right/down, top edge rows).
2. `2_exaggerated_warp_1x_vs_4x.png`: `LM_IND_EXP=3` (32 texels) at internal resolution 1x and 4x,
   same frame: identical displacement; `2b_..._16x9.png` the same at 4x 16:9.
3. (replaced by 7 and 8)
4. `4_painting_before_after_diffx16.png`, 6. `6_copy_precision_before_after_diffx16.png`: main build
   (before) vs patch 0014, same frame: RGB5A3/RGB565 precision only (difference x16).
5. `5_event5_bathroom_mirror.png`: event 5 running in the bathroom (no visible warp).
7. `7_console_vs_port_1x_vs_4x.png`: the console capture (left; a low-resolution video capture,
   so softer) next to the port at internal resolution 1x and 4x, same frame (window captures
   `cap_1x.png`, `cap_4x.png`). Images 1, 2 and 4 predate patch 0015 (painting without scatter).
8. `8_console_vs_rules_zero_generate_tc0_generate4x.png`: console, then `LM_TEXGEN_BEYOND` 0, 1,
   2 and 1 at 4x (native frame dumps, same frame).

### Test aids

- `LM_IND_LOG=1`: each frame's draws with indirect stages, grouped by guest call chain and state
  (indirect/TEV stages, texmaps with image address/size/format and the first indirect texture bytes,
  texgen matrix, indirect matrices, `numTexGens`, RGB5A3 texel classes), printed when a frame's set
  changes. Combine with `AURORA_GX_AUDIT=1` for state from display lists.
- `LM_IND_OFF=1` (indirect never enabled), `LM_IND_EXP=<n>` (matrix exponent + n),
  `LM_IND_PATTERN=1` (left half of the indirect texture intensity 15, right half 0),
  `LM_TEXGEN_BEYOND=0|1|2` (texcoords beyond numTexGens: (0,0) / generated (default) /
  texcoord 0), `LM_TEXGEN_LOG=1` (each draw that reads a texcoord beyond numTexGens, once).
- Reaching the effects: event 6 `LM_DEV_WARPS=2:9@2800` with `[debug] warp_skip_events=0` in
  settings.ini; event 5 `LM_TELEPORT=-1648.7,550,-4889,200` from the foyer of a new game.

## Shadows

**Symptom (user).** No real-time shadows anywhere: the flashlight cast no shadows of furniture,
bears or Luigi onto walls and floors, and lightning flashes lit rooms without the big shadows
they throw. In Dolphin with a widescreen hack, shadows stop at the 4:3 area.

### How the game does it (US addresses; one technique for every dynamic shadow)

All real-time shadows come from **one depth shadow map** per frame, rendered from the current
light and compared per pixel in the TEV. Code in `auto_03_8005D3A4` (shadow object
`lbl_803CFC0C`, functions `fn_8005D41C`..`fn_8005DF84`):

1. **Light selection** (`fn_80029050`/`fn_80029488`, `fn_800293A8`): normally the flashlight
   (eye/target from Luigi's hand, fovY/near/far into the camera); during a **lightning strike**
   the room's thunder light. The room update `fn_800168EC` starts a strike when the room info's
   `Thunder` field (`fn_8002D7C8`) is non-zero and a timer (`fn_8000FC60`) passes 1.87
   (`lbl_804D9B30`); `fn_80029250` then switches the light for 60 frames and sets the shadow
   strength to 150 (K3 alpha). Cutscenes (`fn_80114144`) can set the light from keyframes
   (`fn_80116460`).
2. **Caster pass** (`fn_8005EF84`, every gameplay frame while the shadow is enabled):
   `fn_8005D984` turns the main camera `lbl_803A3820` into the light's camera (perspective
   `MTXPerspective(fovY +0xA0, aspect +0x94, near, far)`, or orthographic), viewport 640x480,
   scissor (1,1)-(639,479), sets `lbl_804D8080 = 1`, and builds the receivers' matrices:
   `+0xB0` = `MTXLightPerspective` (y flipped) x light view, `+0xE0` = a depth-ramp matrix whose
   s/q is the light-space window depth and t = 256 s (`fn_8005D7B8`). Furniture, actors and Luigi
   are then drawn into the EFB from the light (`fn_80011824`, `fn_80060D44`, `fn_801588C8`,
   `fn_80072230`).
3. **Copy** (`fn_8005DBE4` -> `fn_8005D8CC`): `GXCopyTex` **Z16** 640x480 with clear (color 0,
   Z = far) into the shadow buffer, camera restored, `lbl_804D8080 = 0`. The buffer is read as an
   **IA8** 640x480 texture (`fn_8005D41C`, clamp, LINEAR): I = depth bits 23..16, A = 15..8.
4. **Receivers** (rooms `fn_80015CF4`/`fn_8001566C`, actors `fn_80060E70`, Luigi/J3D
   `fn_8005A550`, cutscene objects `fn_801142C4`/`fn_801144C4`): `fn_8005DD50` (plus
   `fn_8005DCA0` for room geometry, `fn_80058A60` adds per-vertex `TEX0/1MTXIDX` for skinned
   models) sets
   - texcoord 0 = POS x (ramp matrix) -> 256x256 IA8 **depth ramp LUT** (`fn_8005D560`: texel
     (x,y) = A x, I y; s clamped, t repeating), texcoord 1 = POS x (light projection) -> the
     shadow map; both projective 3x4 (dual-tex post matrices PT18/PT19 for actors);
   - TEV stage 0: PREV = ramp texel through swap table 1 (R = I, G = A);
   - TEV stage 1: shadow map through swap table 2 (R = A, G = I); **alpha op `GR16_GT`**:
     REG0.a = (PREV.GR16 > TEX.GR16) ? K3.a : 0, i.e. "receiver is deeper than the map" ->
     shadow strength (0xA4, 0x96 for lightning);
   - the material's own stages follow; two final stages (`fn_800154E4` for rooms) multiply the
     strength by the light's **ALPHA1** channel (lit only by the shadow light, spot attenuation)
     and lerp the color toward K3 (near black): shadows only appear where that light shines.
   Other "shadow" assets (`/model/shadow.szp`, `bshadow*.szp`, `gshadow.bmd`, `kage_*.mdl`) are
   ordinary ghost/boss models drawn as regular geometry; no other projection or stencil
   technique exists.

### Root causes and fixes

| # | Cause | Effect | Fix |
|---|-------|--------|-----|
| 1 | **TEV color and alpha units did not read their inputs simultaneously.** Aurora wrote the stage's color result to its register before generating the alpha op; the shadow stage's alpha `GR16_GT` compares the *color* inputs A = PREV and B = TEXC, and the color op of that same stage had just overwritten PREV with 0 (its own compare result). The alpha compare saw 0 > map and was never true. Same bug in the integer and float TEV paths. | No shadow anywhere (flashlight, lightning) | Patch 0011 (`shader.cpp`): both units compute into temporaries (`tevcN`, `tevaN`) and store afterwards, as on hardware. Verified with a shader debug view: compare result now equals (ramp key > map key) on every pixel. |
| 2 | **Widescreen/FOV projection applied to the light pass.** The host GX layer scales every full-viewport perspective projection (Hor+, FOV slider; orthographic: centered 4:3). The light's view got squeezed while the receivers projected the map with the game's unmodified matrices. | Shadows misplaced/missing at 16:9+ | `hle_gx.cpp` `drawMode`: while the game's shadow-pass flag `lbl_804D8080` is set, projections stay plain. |
| 3 | **The light frustum only covers the 4:3 picture** (game design; the cause of the Dolphin widescreen-hack symptom). | No shadows near the left/right edges at 16:9..32:9 (e.g. nursery table/lamp in a lightning flash at 32:9) | `shadows.cpp` hooks `fn_8005D984` (US + PAL): during the setup call the camera aspect (+0x94) is divided by the widescreen factor, so the light projection, the receivers' `MTXLightPerspective` and the light's culling planes are all built from the same wider frustum by the game's own code (consistent by construction; 4:3 output unchanged pixel for pixel). `ws::setShadowLightSetup` keeps the view's Hor+/FOV culling adjustment off the light camera. Toggle **Widescreen shadows** (`shadow_widescreen`, on). |
| 4 | **Shadow-map resolution.** The Z16 copy is read as IA8, so EFB-copies-to-RAM decoded it from the console bytes at 640x480 even at high internal resolutions (blocky, stair-stepped shadow edges and coarse self-shadow stripes). | Low-res shadows at 2x-8x | `efb_ram.cpp`: a Z16 copy read as IA8 of the same size/address uses Aurora's copy texture at the internal resolution (it holds exactly the IA8 view: I = bits 23..16, A = 15..8). Toggle **Shadow map at render resolution** (`shadow_render_resolution`, on; off = console 640x480 bytes). Without "EFB copies to RAM" the map always follows the render resolution. |
| 5 | **MSAA crashed** at the first depth copy ("Depth tex copies from multisampled EFB targets are not supported"; the game copies Z16 every frame, so any MSAA setting aborted at boot). | Crash with MSAA | `tex_copy_conv.cpp`/`encoding.cpp`: depth-copy pipelines for multisampled depth (sample 0). |

Not causes (checked): projective texgen with post-transform matrices and per-vertex texture
matrix indices, the q divide, swap tables, the ALPHA1 spot channel, the LUT/shadow-map texture
decode and the Z16 copy byte order (I = high byte, matches the game's swap/compare setup),
interpolated frame rates (the shadow pass is replayed with the rest of the frame; shadows match
Original 30), the copy-to-RAM decode path.

Accuracy notes: the game samples both packed textures (LUT and map) with LINEAR filtering,
which mixes the high/low depth bytes independently; the faint diagonal stripes this causes on
self-shadowed curved surfaces (the nursery bears) are part of the hardware technique and become
finer at higher shadow-map resolutions. Aurora filters in float and the integer TEV rounds the
result to 8 bits (hardware: fixed-point weights), a +-1 LSB difference in the low byte.

### Test aid: `LM_LIGHTNING=<first>[,<period>[,<type>]]`

`src/recomp/lightning_test.cpp` (US): counts room updates (`fn_800168EC`, one per gameplay
frame) and forces a strike on update `<first>` and every `<period>` after it (lowers the timer
threshold for that call); with `<type>` a room without thunder uses that thunder type. Logged as
`[lightning] forcing a strike at room-update call N`. Example (nursery, flashes every ~2 s):
`LM_LIGHTNING=0,60,1 LM_TEST_STATE=poltergust LM_START_APPEAR=1 LM_INPUT_CLOCK=frames`.

### Verification (US unless noted; frame dumps via `LM_FRAME_DUMP`, window captures)

- Nursery, flashlight on the crib (frame 2950 of the deterministic nursery script): before, the
  lit wall has no shadows; after, the bears, crib bars and Luigi's head cast shadows onto the
  wall and the bears self-shadow. Forced lightning: Luigi, crib and bears throw long shadows
  across floor and walls.
- 4:3 / 16:9 / 21:9 / 32:9 lightning frames: the shadows in the 4:3 area are identical; at 32:9
  the lamp, vase and chair at the right edge now cast shadows (missing without the widened light).
  Flashlight at 21:9: shadows only with fixes 1+2.
- 4x internal resolution: render-resolution map vs 640x480 map (window captures).
- Original 30 vs Interpolated 60 (2x): same lightning shadows. MSAA 4x: shadows as without MSAA.
- PAL build (GLMP01) at 21:9: flashlight shadows in the nursery (hooks and `VA()` addresses map).

Cost: fix 1 none; fix 2/3 none (a different projection/aspect); render-resolution shadow map
saves the per-frame 640x480 encode+decode (a full-resolution IA8 sample instead); MSAA depth
copy: one extra full-screen pass only with MSAA.

Remaining: the orthographic shadow light (fixed extents in `fn_800085D8`) is not widened (not
seen in tested scenes); no reference capture from real hardware to compare shadow darkness or the
LINEAR-filter stripe pattern exactly; "Shadow map at render resolution" off still uses the scaled
copy when "EFB copies to RAM" is off.

### Shadows at any distance (enhancement) and the room shadow light

**Original behaviour.** Which light casts the shadow map is chosen per frame by `fn_80029050`:
in a dark room (room light object `lbl_803A3CA0` +0x24, dark flag +0xAC set, mode +0x94 == 5)
it is the flashlight (its frustum is the beam: shadows only show where it shines). In a lit room
(after it is cleared, hallways, the training room) and during lightning it is a light that
follows Luigi (`fn_80028CF0`/`fn_80028B74`: target 300 above Luigi, eye at a table distance
toward the upper part of the room, `fn_80029488`) with fov/near/far from the light table
(`fn_800293A8`; e.g. fov 17, near 3000, far 5000 in the 1F rooms and hallways; lightning fov 31,
near 619, far 2322). Shadow strength is K3 alpha: 0x5C lit rooms, 0x96 lightning, 0 when the
light object's +0x89 flag disables shadows. Only what lies inside that small frustum casts and
receives shadows, so in big rooms the shadows of objects away from Luigi (the ballroom's chairs
and curtains at the back wall) are missing and appear/disappear as he walks: the game's own
limit, not a port bug. (`LM_SHADOW_LOG=1` logs the light camera and strength.)

**Enhancement** (`src/recomp/shadows.cpp`, hook on `fn_8005D984`; F1 > Visuals > Rendering
"Shadows at any distance", `[rendering] shadow_any_distance`, on by default; off leaves every
value as the game set it). For the gameplay room/lightning light (detected through
`fn_800293A8`; the flashlight and cutscene lights are untouched) the light keeps its position
and direction, and its fov, aspect and near/far are refitted so the frustum contains its own
frustum plus the receivers that matter: the player's view frustum sampled from 1/4 to 4x the
camera-target distance, **clamped to the current room's bounds** (room table `lbl_804D8170`,
0x30 per room, int min/max xyz at +0x8/+0x14), so the map is no wider than the visible part of
the room (texel size stays close to the original; near is never below 1/4 of the original for
Z16 precision). The game then builds the light projection, the receivers' light/ramp texture
matrices (`+0xB0`/`+0xE0`, used by `fn_8005DCA0`/`fn_8005DB90`) and the light's culling from those
fields itself, so the map and the TEV compare stay consistent.

Why no cascades/second map: the case that looked uncovered (foyer, light "near the camera")
turned out to be the flashlight's beam frustum (dark foyer) and the lightning light, both
handled; in every lit room tested the room light sits far out in front/above (about 3000-4000
units), so one perspective map fitted to the room covers everything visible.

**Test aid** `LM_ROOMS_LIT=1` (US): rooms behave as lit for the shadow light choice (the dark
flag is cleared for the `fn_80029050` call; ambient and lamps are not changed). With
`LM_TELEPORT=x,y,z,frames` (room centres from the room table: ballroom 1800,0,-1400; dining room
-1600,0,-1400; 1F hallway -2400,0,-985; foyer 0,0,200) and a scripted walk.

**Verification** (16:9, frame dumps, `build/shadows2/`): a temporary coverage overlay (green =
inside the shadow map, red = outside) in the lit ballroom shows the back wall, curtains and chairs
outside the map with the option off and the whole room inside with it on (`cov_rooms.png`
rows 1-2, `cov2.png`); the dining room has a small uncovered sliver off, none on; the lit foyer
and the 1F hallway are fully covered either way (`hcov.png`). Rendered frames: ballroom chairs at
the back gain their shadows, the long shadows near Luigi are unchanged except edge pixels
(`rooms_onoff2.png`: off / on / difference, red = darker with the option on);
`ballroom_walk_off_on.png` is a walk through the room (left off, right on).

### Shadow polish (shadows-3)

Checked in the nursery (flashlight, `LM_LIGHTNING`), the ballroom (dancing ghosts, ghost lights,
lightning), lit rooms (`LM_ROOMS_LIT`: ballroom, dining room, foyer, 1F hallway), with enhanced
lighting off and Ultra, 30/60/120 fps, render scale 1x/3x, 16:9/21:9, MSAA 4x and the 2-player
mode (`LM_COOP=1`); images in `build/shadows3/`.

- **Interpolation**: `LM_INTERP_TEST=4` at 60 and 120 fps through flashlight sweeps and the
  flashlight -> lightning switch: every late replay is identical to its 30 Hz frame (0 stale
  pixels), the in-between steps show the shadow map and its receivers consistently (the caster
  pass is part of the same tape as the receivers), no popping spikes in the frame-to-frame change
  (`interp60_lightning_switch.png`). The light switch itself snaps from one light to the other, as
  in the original.
- **MSAA 4x, 21:9, 2-player split screen** with Ultra: shadows drawn, no errors
  (`matrix_base_msaa_21x9_coop.png`).
- **Shadow look** (user report: ghost-light shadows overpower the colored lighting, near-black
  blocks): see "Shadow look" in the parts table. Ballroom and nursery before/after:
  `ballroom_shadowlook_old_new.png`, `ballroom_ghostlights_old_new.png`,
  `nursery_shadowlook_old_new.png` (left 1.0/0 = previous look, right defaults 0.9/0.25): the
  ghosts' orange/white light now shows inside shadows, shadows are a little lighter, the rest of
  the picture is unchanged. The exact close-up in the user's screenshot (camera next to the
  dancers) was not reproduced with the test aids; it most likely comes from the ballroom's
  cutscene camera, where the shadow light is the scripted light of the cutscene.
- **Any-distance fit stability**: the refitted room-light frustum is snapped to 6% steps in
  field of view and 10% in depth (rounded outward), so the shadow map's texel size and depth
  scale do not change every frame with the camera (edge shimmer); the light itself still follows
  Luigi as in the original.
- **Build**: CMake's Aurora patch re-apply now drops the submodule index before checking files
  out again: `checkout-index -f` left stat-clean CRLF files from an older autocrlf checkout
  untouched and every patch then failed to apply.

Not changed / remaining: shadows on translucent ghost bodies are the original behaviour (the
actor pass gives ghosts the shadow stages too); the jagged edge of a grazing light's shadow is
the shadow map's own resolution along the receiver (raise "Shadow map size"); the 2-player mode's
second view uses the shadow map fitted to player 1's view.

### Stable shadows with many lights (shadows-4, ballroom dance)

User report: in the ballroom dance (ghost lights, Ultra) shadows flickered, overlapped and
vanished, and the flashlight on the ghosts made everything "wonky". Measured with consecutive
frame dumps (`LM_TELEPORT=1800,0,-1400,100`, Ultra, ghost lights, ambient 0.35, 120 fps target,
frames 2720-2840, standing still and with a scripted flashlight sweep across the dancers;
`LM_SHADOW_LOG=2` logs the shadow light every frame). The game's own shadow light there is the
flashlight the whole time (no switching except lightning); the popping came from the colored
light side:

| Cause | Fix |
|---|---|
| Ghost light intensity jumped between game frames (dance visibility toggles, appear flares, a per-frame random "nervous" flicker while in the beam), so pools, their shadows and the bloom popped; the room's mean brightness jumped by 10% in single frames | `ghost_lights.cpp`: submitted intensity smoothed per ghost (attack ~3, release ~5 frames); the beam/struggle flicker uses smooth noise (keys every 6 frames, smoothstep in between) |
| The two shadow-casting ghost lights were re-chosen every frame by a score that changes as the dancers move: shadows appeared and vanished | Hysteresis (a light that casts a shadow keeps it unless another is 35% stronger) and per-light shadow weights that fade over ~5 game frames, sent in `ExtraLight::flags` bits 8..15 (0 = full); `interp.cpp` blends the weight between game frames; Aurora uses up to 4 slots so fading shadows fit (patch 0020) |
| Screen-space light shadows: a binary per-pixel occlusion test with a step jitter that changed every rendered frame (crawling, noisy edges, different on each interpolated replay) | Patch 0020: the jitter is fixed per pixel, occlusion is soft (ramps over the thickness and toward the light), and the receivers sample the half-resolution mask with four bilinear taps (4x4 tent) |
| The game's shadow light switching (flashlight <-> room light / lightning / cutscene light) popped the new light's shadows in | `shadows.cpp` (enhanced lighting only): after a switch (or a jump of the light by more than 1500 units) the shadow strength (K3 alpha, read by `fn_8005DD50` after the pass) fades in over ~6 game frames. The old light's shadows still end at once (one shadow map per frame in the game) |

Per-light combination: each extra light's shadow removes only that light (mask × weight on its own
term), the game's shadow keeps the extra lights and part of the ambient (0019), so overlapping
shadows never double-darken a pixel lit by another light.

Results (`build/shadows4/`): temporal flicker (mean |L(t-1) + L(t+1) - 2 L(t)| / 2 over the room,
HUD excluded) with the sweep 4.57 -> 4.11 (dark pixels 2.50 -> 2.24, pixels above 8 levels
11.5% -> 10.3%); standing still 3.28 -> 3.21 (the dancers' own motion dominates); the room's mean
brightness no longer jumps (sweep: 44.4 -> 27.0 -> 48.0 before, 43.9 -> 44.7 -> 48.2 after).
Strips of consecutive frames, top before / bottom after: `strip_sweep.png`, `strip_still.png`,
`strip_lightning.png`. Master switch off: frame-identical to the previous build (nursery
lightning, 10 frames).

Not done: separate shadow maps per light with a true cross-fade of the game's light (the old
map is gone the moment the game renders the new one); a temporal filter of the screen-space mask
(the static pattern + blur made it stable enough at 30 and 120 fps).

### Shadows and frame interpolation (shadows-5)

User report: waving the flashlight at a ballroom chair, its shadow jittered quickly; only with
frame interpolation (Original 30 stable). The user's clip (120 fps) shows the pixels at the edge
of the chair's shadow on the wall going back and forth every other presented frame
(`build/shadows5/user_clip_oscillation_map.png`, red = back-and-forth pixels).

**Cause.** In an interpolated replay the shadow map is rendered from the caster pass with its
matrices blended (light projection element-wise, light-view x model rigidly: slerp), while the
receivers' shadow texture matrices (light projection x model, fn_8005DCA0; post matrices
PT18/PT19, fn_8005DD50) are blended element-wise as projection-style texture matrices. With
the light turning quickly the two blends give different in-between lights, so the shadow is
looked up in a map from a slightly different light: the shadow slides away and back inside
every game frame.

**Fix** (`shadows.cpp`, `interp::pushNoBlend`): the caster pass (fn_8005D984 .. fn_8005D8CC)
and the receivers' shadow texgen (fn_8005DCA0, fn_8005DD50 hooks, US + PAL) record no blend
patches, so they replay exactly as captured: the shadow map, its light projection and the
receivers' lookup always come from the same game frame. Shadows then move at the game's 30 Hz
(as in Original 30) while everything else stays smooth. Original 30 is unchanged (no tapes are
recorded there; plain-renderer frame dumps identical).
`LM_SHADOW_SNAP=0` restores the old blending, `=2` blends the caster pass element-wise instead
(smooth but only half as consistent).

**Superseded (interp-everything, docs/INTERPOLATION.md):** the default is now `LM_SHADOW_SNAP=3`:
the caster pass is blended as blended light camera x blended caster world matrices, and the
receivers' matrices are rebuilt from the same blended light (K = P x LV from the shadow object,
times each receiver's own G), so the shadows move at the presented frame rate and stay consistent
(back-and-forth pixels over the nursery scenario at the level of the snapped mode). `=1` keeps the
30 Hz snap.

Measured (`LM_INTERP_TEST=4`, 120 fps, ballroom `LM_TELEPORT=1800,0,-1400,100`, Ultra, Luigi
turning left/right every 4 game frames; the shadow term isolated as the difference to a run with
`LM_NO_SHADOW=1`; pixels whose shadow goes back and forth within one game interval): blended
4004 px per interval, snapped 631 (run-to-run noise floor), linear caster pass 1226.
`chair_shadow_only_strip.png`: shadow term over two game frames (each 4 replays), top before
(blobs that appear only in the in-between frames), bottom after.

Test aids added: `LM_NO_SHADOW=1` (shadow strength 0), C-stick tokens `CUP/CDOWN/CLEFT/CRIGHT`
in `--input`.

## Debug draw overrides (Aurora patch 0012)

`patches/aurora/0012-wireframe-override.patch`, used only by the model viewer
(docs/UNUSED_CONTENT.md, "Model viewer"): `GXSetWireframeAurora(enable, rgba)` draws every
triangle-type draw (also the display-list optimiser's `GX_AURORA_DRAW_INDEXED` draws) as a
line list of its triangle edges in one colour (no culling, no depth bias, no draw merging
while on; WebGPU has no polygon fill mode), and `GXSetCullOverrideAurora(twoSided)` forces
GX_CULL_NONE. Both are FIFO commands (`GX_AURORA_SET_WIREFRAME` 0x0050,
`GX_AURORA_SET_CULL_OVERRIDE` 0x0051), so they stay in order with the draws and in the
interpolation tape. The game never issues them: pipelines and shaders are unchanged unless a
viewer turns them on (`ShaderConfig::wireframe` uses a former padding bit, so the pipeline
cache keys of existing pipelines do not change).

## Pipeline precache (Aurora patch 0013): no first-run flicker

**Symptom (user):** on the first boot many things flickered in and out while playing; later
runs were fine.

**Cause.** Aurora builds a WebGPU render pipeline (WGSL generated from the GX TEV/vertex/
indirect state, plus blend/depth/cull state) for every distinct GX state the game draws with,
lazily, on a background thread. `gx::render` skipped any draw whose pipeline was not ready
yet (`get_pipeline` returned false and the draw was dropped), so for the frames it took to
compile (about 50 ms per pipeline with a cold driver cache, 99 pipelines from boot to the
foyer) objects, effects and HUD parts were missing. Measured on a cold run boot -> foyer with
the old behaviour (`LM_PIPELINE_WAIT=0`): **5118 dropped draws**. Later runs were fine
because Aurora remembers every config in `pipeline_cache.db` and rebuilds them all at startup,
and Dawn's blob cache (`dawn_cache.db`) makes that fast (about 2 ms per pipeline). Both caches
were persisted correctly and keyed by content (xxh3 of the config struct; no pointers), so
they hit on the next run.

**Fix** (`patches/aurora/0013-pipeline-precache.patch`, `src/recomp/precache.cpp`):

- **Draws never drop.** `get_pipeline` waits for a pipeline that is queued or compiling (moves
  it to the front of the queue) instead of skipping the draw; only a pipeline whose creation
  failed is skipped (`droppedDraws` stat). Worst case is a hitch, never a missing object.
- **Several compile threads** (half the hardware threads, 1..8; `LM_PIPELINE_THREADS`). The
  shipped list compiles in about 3 s with 8 threads vs 13 s with one (warm driver cache).
- **Shipped precache list** `data/pipelines.bin` (copied next to the exe, installed with it):
  5453 pipeline configs (79 KB, zstd) recorded by an automated sweep. It holds Aurora's
  config structs only (generated metadata, no game data). `aurora_pipeline_cache_seed` adds
  them to the known set on every launch; Aurora's own `pipeline_cache.db` adds whatever this
  PC used beyond it. The known set is compiled in the background from the **first frame**
  (Aurora used to start before the application could add a list or set its accuracy flags),
  in first-use order (title screen first).
- **Setup compiles it with a progress bar** ("Compiling shaders", docs/SETUP.md), with a
  "Play now" button that leaves the rest to the background. On later launches whatever is
  missing (deleted cache, driver update invalidating Dawn's cache, new game version) compiles
  in the background while the title runs, with a small "Compiling shaders n / N" note in the
  corner; draws that need a pipeline before it is done wait for it.
- **Settings that change shaders.** The F1 > Visuals > Rendering accuracy toggles (integer
  TEV, EFB colour depth) are baked into each shader config. Aurora derives their variants from
  the recorded configs (`intTev` from the flag; EFB depth off clears `efbFmt`/`dither`/
  `efbBlend`, exactly as `populate_pipeline_config` does) and rebuilds the set in the
  background when a flag changes, so the list (recorded with both on, the defaults) covers
  every combination. MSAA only changes the render target layout, which Aurora already
  rebuilds the whole set for. The CRT filter is a post-process chain outside the pipeline
  cache; the wireframe override (patch 0012, model viewer) is not precached.
- The Dawn cache prune (deletes blobs not used in a session) now runs only after the first
  full rebuild was queued; with several threads and the deferred rebuild it could otherwise
  fire while the queue was momentarily empty and delete blobs still needed.

**Measurements** (US, dev build, RTX-class GPU, Original 30, 8 threads; "worst frame" is the
worst frame time per 5 s window, first window excluded since it contains window creation):

| Run | Pipelines created on demand | Draws that waited | Dropped draws | Worst frame |
|-----|------|------|------|------|
| Cold, old behaviour (`LM_PIPELINE_WAIT=0`, no list), boot -> foyer | 99 | 0 | **5118** | n/a |
| Cold, no list, waiting (`LM_PRECACHE=0`), boot -> foyer | 99 | 35 (1.4 s total, max 111 ms) | 0 | 224 ms |
| Warm (second run, own cache), boot -> foyer | 0 | 0 | 0 | 81 ms |
| Fresh AppData with the list, boot -> foyer | 0 | 0 | 0 | 83 ms |
| Fresh AppData with the list, all rooms in reverse order with the water element (not a sweep route) | 4 | 1 (0.2 ms) | 0 | 77 ms |
| Fresh AppData with the list, accuracy toggles off (`LM_ACCURACY=0`) | 0 | 0 | 0 | 36 ms |
| `LuigisMansionRecomp` fresh install (setup -> game) | 0 | 0 | 0 | 51 ms |

### The sweep (`tools/pipelines/sweep.py`)

Regenerate the list after Aurora changes that alter `gx::PipelineConfig`/`ShaderConfig`
(bump `GXPipelineConfigVersion` when the layout or meaning changes; rows of another version
or size are ignored, so a stale list only costs coverage, never correctness):

```
python tools/pipelines/sweep.py --work <scratch dir> --jobs 4 [--merge data/pipelines.bin]
```

It runs `lmrecomp` with a scratch APPDATA per scenario (in parallel, each started without a
console window and 8 s apart; a run that crashes on its own is repeated, `--resume` keeps the
scenarios an earlier sweep into the same `--work` finished), `LM_PRECACHE=0`, F1 options from a
`settings.ini` written into that APPDATA, and merges each scenario's `pipeline_cache.db` with
`tools/pipelines/pipelines.py build`:
boot (title, menus, file select, the opening unskipped, foyer), the same with cutscene skip,
a teleport tour of every mansion room (points from the map's `jmp/furnitureinfo`, computed from
the disc dump at sweep time: `tools/pipelines/rooms.py`) with vacuum/blow/A/stick input and
the Game Boy Horror opened every 8 rooms, the tour with fire/water/ice, the Hidden Mansion
with USA and PAL rules (mirrored), the unused-content shader variants (early ghost shading,
GBH live camera), and maps 1, 3-13 (lab, training, Portrificationizer, unused 5/7/8, gallery,
roof/Bowser, Chauncey, Boolossus, graveyard/Bogmire). About 20 minutes; per-scenario counts:
boot 113, tour 757 (the tour ends at the roof and loads Bowser's map), fire 726, water 950,
ice 445, Hidden Mansion 660/664, unused 280, maps 75-145 (graveyard 677).

Second set (2026-10-11, `--only enh_,boss,appear,ending,unused,coop,training,viewer`; about
an hour per release with 3 jobs; the machine's commit limit, not the GPU, caps the jobs):

- `enh_*`: **Enhanced lighting** with every part on (Custom preset, per-pixel lights, PCSS,
  colored-light shadows as cube shadow maps, volumetric beam, bloom, forward AO, ghost lights,
  world lights): the whole tour (1169 / 945 after Aurora 0031/0032), each element, the Hidden
  Mansion both rule sets, boot, maps 0, 1, 3-8, 12, the training room and the boss maps;
  `enh_lowq` and `enh_water` use the cheap variants (screen-space colored-light shadows, low
  quality). Enhanced lighting's GX-derived draws (enhanced shader classes, glow draws, the
  depth pre-pass, light shadow-map draws) are pipeline-cache configs; its scene-end passes
  (bloom, volumetric, AO, cube clears) and the CRT filter are fixed pipelines outside it.
- `boss09/10/11/13` (+ `enh_`): the boss maps played for 6700 frames with strong vacuum,
  weak ghosts and no damage (`gameplay` options), Boolossus with ice. Blind input does not
  finish the fights (Bowser and Chauncey stay in their opening phase), but it reaches the
  fight effects of Boolossus and Bogmire.
- `training` (+ `enh_`): map 3 with A held through the controller settings screen and
  E. Gadd's text, then the training ghosts.
- `appear9/7/5/3`: mansion entry points whose arrival plays an event (Secret Altar/King Boo,
  Sealed Room, Balcony Boos, roof), with `warp_skip_events=0`.
- `ending_r8`/`ending_r1`: `LM_TEST_ENDING` (rank screen, ending scenes; unused ending
  pictures on in r8).
- `unused_a`/`unused_b`/`unused_enh`/`unused_bogmire`/`unused_portraits`: every unused-content
  and debug toggle (GBH camera/tracker/clock, beta skybox, doors, early Luigi, air/water
  nozzles, early Poltergust, green Toad, bordered view, poison, crawl fear, overheat, ghost
  placements and colour variants, extra Speedy Spirits, depth blur, debug text), Play as
  Mario, early Bogmire, portrait models, with the CRT filter on.
- `coop`, `coop_mario`, `coop_map10/13`: the 2-player split screen (P2 input on port 2),
  Mario as player 2 with Enhanced lighting.
- `viewer`: the model viewer's `LM_DEV_VIEWER_SWEEP` over all 1458 models (289 configs);
  `viewer_mapNN`: whole maps in the viewer (US only).

Both releases ran the set (the European game with `--exe build/pal/lmrecomp.exe --disc
orig/GLMP01.iso`): 4456 configs (US) and 4242 (European game), 2903 from the US
Enhanced-lighting rerun after Aurora 0031/0032; merged with the earlier list: 2148 -> 5453.

Fresh-cache runs with `LM_PIPELINE_LOG=1` (US unless noted; new game -> foyer, walk/vacuum;
MISS = pipelines a draw needed that were not precached; "ready" = `[precache] all N known
pipelines ready after` line, fresh `dawn_cache.db`, warm driver cache):

| Run | 2148-config list | 5453-config list |
|-----|------|------|
| Foyer, default options | 0 MISS | 0 MISS |
| Foyer, Enhanced lighting + AO + ghost/world lights | 78 MISS | 51 MISS |
| Boolossus (map 11), Enhanced lighting, ice | 101 MISS | 64 MISS |
| Bogmire (map 13), default options | 29 MISS | 0 MISS |
| European game, foyer, Enhanced lighting | 76 MISS | 51 MISS |
| Startup precompile, 8 threads (several runs, machine shared with other builds) | 2.3-6 s | 7-40 s |
| Startup precompile, 1 thread | | 52-59 s |

The precompile runs in the background from the first frame (title screen first), so the
longer list mostly costs CPU time while the title runs; the setup's "Compiling shaders" step
takes correspondingly longer. 2147 of the 5453 configs only occur with Enhanced lighting
(off by default). The remaining Enhanced-lighting MISSes are **not list gaps**: the precache
builds each known config for the scene and offscreen render-target layouts only, while
Enhanced lighting draws known configs into its own targets (glow layout, the depth-only
pre-pass, light shadow maps), and `pipeline_cache.db` does not record the layout.
**Closed by Aurora 0214** (fps-lock, docs/INTERPOLATION.md "Pass 5"): the layout follows from
the config itself - depthOnly / lightDepth variants are built for the depth-only target (only),
glow-shaped ones (additive, no depth/alpha writes, dstAlpha off) also for the glow target. A
10-minute foyer run with enhanced Ultra: 64 MISSes -> 0. (Enhanced-only configs are still built
while it is off.)

`pipelines.py info <list|db>` summarises a list; `aurora_pipeline_cache_export` writes the
current known set in the same format.

### Variants for every settings combination, canonical keys, shaders before the game (Aurora 0217)

**Symptom (user, 2026-10-13):** with 0214 a play session (4K, 60 fps, Enhanced lighting Ultra with
ghost/world lights, AO, realistic flashlight, many shadow lights, CRT, smooth banding) still
compiled pipelines on demand: 34 in the first 5 minutes, **137 over the whole session**
(`out/play2`), several at 100-180 ms, matching visible dips (arriving in the foyer from the lab,
a room with a fire effect, the treasure chest after a room clear, the key-get pose). Boot logos
and the title could also stutter, since the game started while the precache was still running.

**What they were.** The session's `pipeline_cache.db` (1212 configs) against the shipped list
adjusted for the session's flags: 427 configs not in it. By category:
- **The destination alpha value** (about 140, the largest): `PipelineConfig::dstAlpha` holds the
  GX destination alpha value, but the pipeline only depends on whether it is off, 0 or another
  value (the value is the draw's blend constant). LM's ghost pass writes each ghost's fade level
  there, so every fade level of every ghost material was its own pipeline; 4200 of the list's
  5453 configs were such duplicates.
- **Variants the settings derive from a recorded config** (about 200): enhanced shader classes
  (world / ghost pass) of configs recorded with Enhanced lighting off, the depth pre-pass and
  shadow-caster copies (`dc`/`lc`), glow re-draws, the realistic flashlight's alpha-only cone and
  the early-Z alpha-test split pair (0170/0179: never derived, recorded only where a sweep ran).
- **State the shader does not read** (about 50): lighting parameters of color channels no TEV stage
  reads (the game sets the flashlight's spot attenuation on channels a material ignores),
  texgens of unused coordinates.
- **New content** (about 40): materials no sweep reached (the user's lab -> foyer, fire room,
  chest, key-get).
Smooth banding, 6-bit blending and the vertex snapshot are uniforms or passes, not configs: no
gaps from them.

**Fix** (`patches/aurora/0217-precache-variants.patch`, `src/recomp/precache.cpp`, `main.cpp`):
- **Canonical keys.** `gx::canonicalize_pipeline_config` (applied in `find_pipeline` and to every
  known config) reduces `dstAlpha` to off / 0 / other, resets unread color channels and texgens,
  and drops the enhanced class of depth-only copies (they never run the enhanced shading; the
  class only appended uniforms after everything else). Equal shaders share one pipeline: the
  5453-config list is 1117 shaders.
- **Variants generated, not recorded.** The rebuild expands every known config into what the
  current settings produce (`queue_gx_variants`): accuracy flags (as before), then
  `enhanced::precache_variants` for the enhanced-lighting parts that select variants
  (`enhanced::precache_bits`: master switch, emissive bloom, depth pre-pass for point-light
  shadows or AO, shadow-map casters, realistic flashlight, AO receivers) and the early-Z split
  pair (`gx::early_z_split_variants`, shared with the draw path), each for the targets it is
  drawn into (scene / offscreen / 0214's depth and glow targets). The list therefore only needs
  each material once, recorded with any settings; every combination of settings is built from
  it, and a change of those parts rebuilds like an accuracy flag change. Where the draw class is
  unknown it errs towards building one variant too many.
- A config first met in play queues its variants at once (one miss, not one per variant).
- **The game waits for its shaders** (`precache::holdGame`, both executables): the guest does not
  run before every pipeline the startup rebuild queued is built (Aurora stat `rebuildPending`);
  the window shows "Preparing shaders... n / m" (nothing for the first 0.3 s, "Loading..." when
  short). A settings change that queues new variants pauses the game (and its audio) with the
  same overlay instead of stuttering in play. The settings are applied before the first frame,
  so the first build is the right one. `LM_PRECACHE_WAIT=0` restores the background compile.
- MISS log lines name the canonical config, the render target layout and the state (class,
  depth-only/caster, EFB format, blend, depth, cull, dst alpha).
- New list: the canonical union of every config recorded on the development machine (all sweeps
  and test runs of both releases since the config version 15, the user's play sessions
  `out/play1`/`play2`): 4333 configs; with the verification runs below merged in **4387** (78 KB). `LM_PIPELINE_EXPORT=<file>` writes the canonical
  known set (with `LM_PRECACHE_WAIT=0 LM_PIPELINE_EXPORT_EXIT=1` on the first frame and exits):
  `pipelines.py build -o data/pipelines.bin <export>`.

**Startup (fresh `APPDATA`, i.e. empty `dawn_cache.db`; 8 threads, i9-12900KS):**

| Settings | Pipelines | Cold | Warm |
|----|----|----|----|
| Default (Enhanced lighting off), old 5453 list | 1100 | 6.4 s | |
| User's settings (Enhanced Ultra, ghost/world lights, AO, realistic flashlight, shadow maps) | 8969 | 82.9 s | 1.64 s |

(The first Ultra attempt queued 13103 pipelines in 142 s: enhanced classes for every textured
config, the depth copies per class, and a first rebuild for Enhanced lighting off before the
settings were applied. The class test now follows `shader_class`; depth copies are class-free.)

**Coverage check** (`tools/pipelines/verify.py`): scripted routes different from the recording
sweep's (boot + unskipped opening, every room in reverse order with the fire element and the
GBH, ghost captures until the room is clear with the hunting aid, the lab, the King Boo and Boo
balcony entry events, Chauncey, Bogmire, the ending), each with a fresh cache and only the
shipped list, one game at a time (`--lock`), per F1 preset:

| Preset | Routes | MISS (main before 0217, old list) | MISS (0217, 4333-config list; before merging these runs) | Startup, cold |
|----|----|----|----|----|
| User's settings (Ultra, ghost/world lights, AO, realistic flashlight 3, shadow maps, smooth banding, CRT) | opening, tour, ghost room, lab, event 9, event 5, Chauncey, Bogmire, ending | opening 62, tour 147 | **1** (tour; the other 8 routes 0) | 77-86 s (8969 pipelines) |
| Default (Enhanced lighting off) | the same 9 | opening 62, tour 110 | **1** (tour; the other 8 routes 0) | 19-23 s (3043) |
| Enhanced Low | opening, tour, event 9, Bogmire | | 1 (tour) over opening, tour, event 9; Bogmire **0** (4387 list) | 72-79 s (8165); 3 threads on a loaded machine: 298 s (8180) |
| Enhanced High | the same 4 | | **0** on all 4 (4387 list) | 3 threads, loaded machine: 233-374 s (8180) |
| Enhanced Ultra (+ ghost/world lights, AO, realistic flashlight 3) | the same 4 | | **0** on all 4 (4387 list) | 3 threads, loaded machine: 203-325 s (8984) |
| Enhanced Ultra at 120 fps (interpolated, `enh_ultra_120`) | tour | | **0** (4387 list) | 278 s (8984) |

The Enhanced High/Ultra and Low-on-Bogmire runs (2026-10-14, main 3c6c4c7 with Aurora 0219, the High opening on c0a8041; the
4387-config list) ran every route to its end with no MISS; their caches added nothing to the list
(their 28 recorded rows were all in it already: still 4387 configs). Frame interpolation at 120 fps
(tagged interpolation, presented 90-117 fps) needs no pipeline of its own: it replays the recorded
draws with blended uniforms. The startup times are not comparable with the earlier rows: the
machine ran up to six other games and `LM_PIPELINE_THREADS=3` (with 8 threads next to them the shader
compiler ran out of memory). Run them like this (one game at a time, `--resume` keeps finished runs,
`--lock` is the shared game-window lock file if one is in use):

```
python tools/pipelines/verify.py --work <scratch>/vfy --data-root <checkout with orig/> --resume ^
    --presets enh_low,enh_high,enh_ultra,enh_ultra_120 --routes opening,tour,event9,boss13 ^
    --startup-seconds 6000 [--lock <GAME_LOCK file>]
```

A run ends at the route's last guest frame (the input script is timed in presented frames), when
the time limit (route length x 1.4 + `--startup-seconds`) runs out, or after `--stall-seconds`
(600) without log output (a hang; set `LM_HANG_DUMP=120` for the thread stacks). `result.json`
says `"complete": false` when it ended before the route's end: a MISS count of 0 then means
nothing for the rest of the route. Every `<preset>` has a `<preset>_120` twin at 120 fps.
Two runs on c0a8041 hung in the opening (main loop idle, a Dawn thread waiting for a Dawn mutex:
probably the readback-callback / present-timing deadlock Aurora 0219 `9de041b` fixes); none on the
build with 0219.

The miss left in the tour (the same material in every preset) is a COLOR1 material whose
material colour comes from the vertex (no vertex colour: constant white) where every recorded
draw of it used the register: a state no recorded run had reached. The verification runs' own
caches were merged into the list afterwards (4387 configs), which covers it.
Turning Enhanced lighting on in play (`LM_SETTINGS_AT`, from the default settings): the game
held 65 s for 7250 pipelines (cold cache) with the overlay, then resumed with 0 misses; a later
change of the shadow mode needed none. After the startup hold, no draw waited for a pipeline in
any run ("stalled draws 0"); the longest frames left at boot are the guest's own start (0.36 s
before the first picture) and the opening scene's load (0.6 s), not shaders.

Not exercised by the routes: ghost captures and the room-clear treasure (the hunting aid stuns
the gold ghosts but did not capture one in these runs), the key-get pose; their configs come from
the user's play sessions in the list.

**Not covered yet** (these compile on first use, without dropping draws): boss fights past
their first phase (Bowser, Chauncey: blind input does not play them), the credits after the
rank screen, the gallery's portrait battles, menus reached only from a save with progress,
Enhanced-lighting settings combinations other than the two swept (most are uniforms, not
configs), the viewer's wireframe override, and MSAA levels (built from the same configs at
the new layout, at runtime). Merging pipeline_cache.db files from longer play sessions
(`pipelines.py build -o data/pipelines.bin data/pipelines.bin <db>...`) closes the gaps.

### Test aids

- `LM_PIPELINE_LOG=1`: every pipeline creation (precache/demand, compile time), every draw
  that waited, every MISS (needed but not precached), and a summary line every 5 s
  (precache progress, demand-created, stalls, dropped draws, worst frame time).
- `LM_PRECACHE=0`: no shipped list (cold-cache testing); `LM_PIPELINE_LIST=<file>`: another list.
- `LM_PIPELINE_WAIT=0`: Aurora's old behaviour (skip draws whose pipeline is not ready), to
  reproduce the flicker.
- `LM_PIPELINE_THREADS=<n>`: compile threads.
- A fresh scratch `APPDATA` (or `LM_CACHE_DIR`) gives empty `dawn_cache.db`/`pipeline_cache.db`;
  the GPU driver's own shader cache is system-wide and stays warm, so first-ever runs on a new
  machine are somewhat slower than measured here.

### Cache robustness (Aurora patch 0017)

**Problem:** a crash once left a bad pipeline config in `pipeline_cache.db`; every later launch
rebuilt it at startup and crashed again. Aurora trusted whatever the caches held: the shader
generator aborts (FATAL) on enum values it does not know and indexes fixed arrays with
stage/texcoord/texmap ids, a Dawn error is an uncaptured error (FATAL), and Dawn even throws
(terminating the process) when a pipeline is created from a shader module that failed to
compile.

**What happens now** (`lib/gfx/pipeline_cache.cpp`, `lib/webgpu/gpu_cache.cpp`, a few lines in
`lib/gx/pipeline.cpp`):

- **Every cached config is validated before use.** `pipeline_cache.db` schema 2 (older caches
  are dropped): the stored `hash` (xxh3 of the config bytes seeded by the shader type, the cache
  key) is recomputed as a per-entry checksum, then every field the generator switches on or
  indexes with is range-checked (`validate_config`: blend/depth/cull, TEV args/ops/konst/order/
  swap/indirect state, texgens of the texcoords the shader reads, vertex attributes, colour
  channels, alpha compare, fog, padding bytes). Invalid rows are skipped and deleted with a
  `pipeline cache: skipping invalid entry ...: <reason>` line.
- **Pipeline creation is guarded** (`guarded_create`): Validation/Internal/OOM error scopes
  around each creation (Dawn error scopes are per thread, so the compile threads do not see each
  other's errors), and the GX shader module is checked before a pipeline is made from it. A
  failure gives an empty pipeline (its draws are skipped, `droppedDraws`), and a validation/
  compile failure records the config in the `pipeline_bad` table: it is never built again and is
  removed from the cache.
- **Crash journal** (`pipeline_cache.journal` next to the caches, 4 KB, memory-mapped): every
  thread writes the key of the config it is building into its slot while it builds it, and the
  file holds the session phase (startup until the first precache pass finished and 300 frames
  were shown, then running, clean after `aurora_shutdown`). Stores to a shared mapping survive a
  crash of the process, so the next launch knows:
  - configs that were being built when the process died become **crash suspects**: not precached
    any more, still built when a draw needs one (the quarantine is lifted when that works),
    refused after a second crash;
  - a death **during startup** with nothing in flight (e.g. a damaged Dawn blob) deletes
    `pipeline_cache.db` and `dawn_cache.db`; a second consecutive startup death deletes them
    too; from the third, the shipped list is also ignored for that run ("safe mode"). Reaching
    the running phase or a clean exit resets the count.
  Caveat: killing the process during startup (test scripts) counts as a startup crash; it
  costs a cache rebuild or the precache of the few configs that were in flight, never
  correctness.
- **Damaged database files** (not a database, truncated, damaged pages: `PRAGMA quick_check`
  at open, `SQLITE_CORRUPT`/`NOTADB` later) are deleted and recreated, for both
  `pipeline_cache.db` and `dawn_cache.db`, instead of running without a cache.
- **`dawn_cache.db`** schema 3 stores an XXH3 checksum of each uncompressed blob. Dawn asks for
  an entry's size before reading it; the entry is decompressed and verified on that size query,
  so a damaged blob is deleted and reported as a miss (Dawn rebuilds it) instead of being handed
  to the driver.
- **Shipped list** `data/pipelines.bin` format 2: CRC-32 of the payload and of every row
  (`tools/pipelines/pipelines.py` writes it; Aurora still reads format 1). Rows with a bad CRC
  or failing `validate_config` are skipped with a log line; the header's raw size is capped at
  64 MB.

**Tested** (scratch `APPDATA`/`LM_CACHE_DIR`, `LM_PIPELINE_LOG=1`, boot -> opening, each run
closed cleanly unless noted; the corruption scripts are not in the repo): random bytes in a
`pipeline_cache.db` entry (checksum mismatch, deleted); entries with an invalid blend mode, fog
type and depth function and a recomputed hash (validation, deleted); an entry with a 7-component
position attribute and a recomputed hash (valid ranges, WGSL fails to compile: recorded as bad,
not rebuilt next launch; before the fix this terminated the process inside Dawn); wrong schema
version (dropped); a row of another config version (ignored); the db truncated to 40 %, overwritten
with random bytes, or 200 random bytes flipped in its pages (recreated); 40 Dawn blobs damaged
(checksum mismatch or decompression failure: deleted, rebuilt), `dawn_cache.db` truncated /
random / page damage (recreated); shipped lists with 20 rows flipped (CRC), an invalid enum, a
non-compiling row, a truncated file, and a format 1 list; startup kills: one with configs in
flight (suspects recorded, no reset), then repeated kills at 3 s (reset after the first with
nothing in flight and after the second, safe mode from the third, back to normal after a run
that reached the running phase). The real list (2148 configs) has 0 rows rejected by the
validator.

Test aids: `LM_PIPELINE_LIST=<file>` (a damaged list), `LM_CACHE_DIR=<dir>`; the journal is
`<cache dir>/pipeline_cache.journal` (u32 magic "AUJN", version, phase 0 clean / 1 startup /
2 running, consecutive startup crashes, then 64 slots); the `pipeline_bad` table lists recorded
configs (`reason` 1 = failed creation, 2 = crash suspect, `crashes`).

### Skipped draws are logged; shadow casters wait (Aurora 0221)

Every draw dropped for a missing pipeline is counted. Reasons: creation failed, waiting turned off
(`LM_PIPELINE_WAIT=0`), or not queued. Logging is always on:
- every second with a non-zero count, a warning "N draws skipped for a missing pipeline in the
  last 1.0 s (k shadow casters)";
- the first skip of each pipeline, one line: "draw skipped:" / "shadow caster skipped:", the
  pipeline key, the reason, and its config in the MISS format (recorded when a draw asked for the
  pipeline before it was built; "precache list entry" if none did).

Fix: the light shadow-map casters (`render_casters`) used `try_get_pipeline` and skipped a caster
whose pipeline was still compiling, so that light had no shadow in that present. They now wait
like every other draw (the 0013 policy). Reason: the user's foyer capture under 100% CPU load
alternated between shadowed and unshadowed presents of one candle light (docs/INTERPOLATION.md,
"Foyer entry flicker report").

Checked:
- cold cache, `LM_PRECACHE_WAIT=0`, game limited to 2 cores, 60 fps, the foyer intro: 0 skipped
  draws, the sconce light monotonic present by present;
- with `LM_PIPELINE_WAIT=0` the log lists the skips (up to 912 per second at startup, 19 distinct
  pipelines with their configs).

### Enhanced-lighting passes before the first foyer (Aurora 0223)

**User report (v1.0):** "a pretty major performance issue when first loading into the foyer". It
happens only on the first foyer of a session.

**Cause.** The foyer is the first scene with enhanced lighting: the title, the file menu, the lab
and the opening's outdoor shots give it no lights. Its first frame built everything enhanced lighting
draws outside the GX pipelines, on the render thread. That was
the effect module's ten scene-end passes (bloom, volumetric/realistic flashlight, AO, composite),
the shadow-map mask, the tile-flag and glow-depth pipelines (13-160 ms each with a fresh shader
cache), and some 20 render targets. LM_PIPELINE_LOG does not see them: they are not GX pipelines
and not in the precache list. Measured on the first foyer frame of a new game (intro route, user
settings, 1920x1080 window, 120 fps target):

| | fresh shader cache (first launch, new install, cleared cache) | warm cache |
|---|---|---|
| before, other games on the PC | 1552 ms render-worker stall | 50-70 ms |
| before, quiet PC | **532 ms** | 39 ms |
| after (0223), quiet PC | **24 ms** (no creation in the worker) | 20-34 ms |

The light shadow-map atlas also followed the frame's number of shadowed lights (cols x rows from
that count). It was released and reallocated (3072x4096 / 3072x6144 / 6144x4096 depth, 3-4 ms each,
plus a full caster redraw) every time a light gained or lost its slot. That happened 5-6 times while
the intro foyer's candles fade in. It never happened on later arrivals.

**Fix** (`patches/aurora/0223-enhanced-prewarm.patch`):
- `enhanced::start_prewarm`: the precache rebuild (startup, and any settings change that
  rebuilds) also starts a background thread. It builds those pipelines for the latest
  configuration (the composite for the EFB format and sample count). Before the first enhanced
  pass, it also creates the render targets at the current EFB size. The thread is counted in
  `rebuildPending`, so the "Preparing shaders" hold waits for it (32-44 ms warm). Every lazy creation is now
  done under one lock (`g_initMutex`; it holds no lock that a Dawn callback takes). The encode
  functions create only what is still missing. `AURORA_ENH_PREWARM=0` restores the old behaviour (A/B).
- The atlas grid's column count follows the configured number of shadow-casting lights, and its
  rows only grow. Tiles keep their size and content, so frame dumps are byte-identical against
  the old layout (enhanced Ultra, original 30, intro foyer frames 1300-1500, `AURORA_BEAM_FREEZE_TIME=1`).
  `AURORA_ENH_ATLAS_GROW=0` restores the old sizing (A/B).
- Debug aid `AURORA_CREATE_LOG=1`: every synchronous pipeline or render-target creation over
  0.5 ms, with its name and thread. In play only the atlas (once, ~3.5 ms) and the post-FX chain
  remain.

The rest of the first-arrival slowdown was the frame-rate governor (docs/INTERPOLATION.md, Pass 5,
"First foyer arrival").

### Warm starts: a faster complete hold, an honest message (Aurora 0226/0227, issue #6)

**User report (public issue #6, GTX 1660 Super, i7-9700KF, Windows 10 LTSC):** "Preparing shaders"
on every start, about 16 s on the second launch, although the shaders had been built before.

**What was measured** (this PC: i9-12900KS, RTX 3090 Ti, NVMe; `LM_PIPELINE_THREADS=4` as on an
8-thread CPU; the new `[precache] shader cache` log lines; other games running on the PC, so
times vary by about 2x):
- **The caches are reused.** Every warm launch, from `lmrecomp` and from `LuigisMansionRecomp`
  after its setup, the second, the third, and a fresh process on a copy of the folder that is not in
  the OS file cache: **0 misses, 0 compiles**. Dawn's keys are content hashes (no version, time or
  settings order in them); D3D12 pipelines come back from Dawn's cached PSO blobs; cache path, WAL
  and exit were all fine.
- **The hold still re-created the whole known set.** A cached pipeline still costs about 1 ms of CPU:
  WGSL generation 0.05, shader module (Tint) 0.2, render pipeline 0.6-0.8 (D3D12 PSO from the
  cached blob, Dawn's lookups; the cache reads alone 0.2 ms, mostly waiting on the cache lock). The
  hold waited for all of them: default settings 3055 pipelines, 0.7-1.2 s; Enhanced lighting
  Ultra 10076 pipelines, 2.5-5 s (game 4.0-6.9 s after launch).
- **After a reboot** `PRAGMA quick_check` (0017) read the whole `dawn_cache.db` before the first
  pipeline: 2.2 s for the 244 MB Ultra cache from an NVMe SSD (0.19 s when in the OS cache); a
  hard disk or SATA SSD takes several times that. Launch on an uncached copy: 5.9 s to the game.
- **A partly missing cache** costs about what the user saw: 30% of the entries deleted ->
  1973 compiles, 13.2 s (4 threads). The old prune deleted every entry a launch had not read by the
  time the startup batch was built: everything of other graphics settings (switching Enhanced
  lighting or MSAA and back compiled them all again) and anything first read later in a session.
- Cold for reference: default 29-39 s (8 threads), Ultra 160 s; no Dawn cache but the NVIDIA driver
  cache warm: 38.7 s (4 threads).

So on a 4-worker CPU with Enhanced lighting, after a reboot, or after a settings switch, the
"about a second" warm start was 5-16 s.

**Fix** (`patches/aurora/0226-warm-shader-cache-start.patch`, `0227-faster-full-shader-hold.patch`,
`src/recomp/precache.cpp`):
- **The game still starts only once every shader is compiled or loaded** (a requirement: no
  background loading by default). The hold is faster instead (0227):
  - while the game is held for a batch that has not missed the cache, all hardware threads but
    one (at most 16) create pipelines (`aurora_pipeline_cache_set_boost`; a compiling batch keeps
    the default half, the shader compiler needs much memory per thread; `LM_PIPELINE_BOOST=0` A/B).
    Cached pipelines stop scaling at about 12-16 threads (Dawn's device lock, taken by every
    creation call): Ultra took 2.1 s with 6 threads, 1.8 with 8, 1.7 with 12, 1.4-1.6 with 16,
    1.7-1.9 with 23;
  - Dawn cache lookups on a read connection per thread (WAL readers run in parallel) instead of
    one connection behind a mutex; writes, deletions and resets stay on the main connection;
  - GX shader modules are reused within a session (shader config, alpha mode, normal attachment):
    about a quarter of the pipelines share their module with another (blend/depth/cull/target
    variants), saving its WGSL generation and Tint parse.
- **Test aid `LM_PRECACHE_WARM=1`: start after a warm sample.** `precache::holdGame` tracks each
  rebuild batch (Aurora stat `rebuildCount`). Once 256 of its pipelines were built without a single
  cache miss (and the enhanced prewarm is done), the game starts and the rest loads in the
  background in first-use order (a draw that needs one first moves it to the front and waits
  about 1 ms, 0013); 8 misses hold it again. Not the default.
- **Age-based prune.** `dawn_cache.db` counts launches (`aurora_session`) and stamps each entry
  with the last launch that read or wrote it (`last_used`, added to existing caches); an entry is
  dropped after 5 launches unused. Entries first used after the prune are stamped at shutdown.
- **No full-file check on a clean start.** `quick_check` (both databases) only runs when the
  previous session did not exit cleanly (crash journal); entries keep their checksums and SQLite
  corruption errors still reset the file. Instead a background thread reads `dawn_cache.db`
  sequentially into the OS file cache (232 MB in 237 ms), so the random blob lookups do not wait
  for the disk one at a time.
- **An honest message.** The hold says "Loading..." / "Loading shaders" while it reads the cache and
  "Compiling shaders for your graphics card" only when the cache misses, with the reason: first start
  with this cache, cache from another graphics card or driver (most lookups miss), some shaders new,
  new graphics settings, or a cache file that cannot be used (then it really happens at every
  start). The corner note during play only appears while shaders are compiled.
- **Log lines for reports**: `[precache] shader cache at game start / after loading: N loaded,
  N missed, N compiled and stored (file: N entries at start, launch N); thread time: WGSL,
  modules, pipelines (cache reads, writes)`, `Dawn shader cache <path> opened (session N, N entries)`,
  `prefetched`, and a line when most cached shaders do not match (another GPU or driver).

**After: the complete hold, warm** (`LuigisMansionRecomp`, same PC with other games running, two
runs each; "hold" = shader preparation, "start" = process start to the first game frame; the
"base" thread count is the worker default, 4 as on an 8-thread CPU or the default 8 here):

| Settings, base threads | Before (0226 only) | 0227, no boost | 0227, boost (16 threads) |
|----|----|----|----|
| Default (3055 pipelines), 4 | hold 0.74-1.24 s | hold 0.64-0.97 s | hold 0.43-0.55 s, start 1.3-1.5 s |
| Default, 8 | | hold 0.46-0.51 s | hold 0.43-0.47 s, start 1.3-1.4 s |
| Ultra (10076), 4 | hold 2.5-5.05 s, start 3.6-6.9 s | hold 2.4-3.4 s | hold 1.66-1.90 s, start 2.6-2.9 s |
| Ultra, 8 | | hold 1.48-1.93 s | hold 1.31-1.68 s, start 2.1-2.7 s |

An 8-thread CPU like the reporter's boosts to 7 threads. Without the full-file integrity check an
uncached ("after a reboot") start no longer reads the whole file first (Ultra: 5.9 s -> 1.3-1.5 s
to the game with the warm-sample test aid; the complete hold adds the 1.3-1.9 s above). Thread time
of a warm Ultra hold: WGSL 0.4-0.5 s, modules 1.6-1.9 s (2660 of 10076 reused), pipelines 5.6-7 s,
of which cache reads 1.1-1.5 s (were 1.7-3.9 s on the shared connection). Cold starts (any miss)
keep the default thread count: default 48 s with 8 threads.

Other cases (0226):

| Case | Before | After |
|----|----|----|
| 30% of the cache deleted | 13.2 s held | the same: held, "Compiling" (1973 misses) |
| Ultra -> default settings -> Ultra | default compiled 3242; the prune then deleted the Ultra-only entries | the same compile once; Ultra again: 0 misses |
| Warm-sample test aid (`LM_PRECACHE_WARM=1`), Ultra | | start 1.1-1.2 s, background 2.4-2.9 s; no stalled or dropped draws, the slow frames are the game's own boot frames 1 and 67/68 |

Note: `AURORA_NO_DEVICE_SYNC=1` changes the device's features and with them every Dawn cache key:
a run with it compiles everything (333 s here) and doubles the cache file until the prune ages the
other set out.

### A start after a crash or a kill (Aurora 0231)

**Observed (timing agent):** after a run was killed, the next start with Enhanced lighting sometimes
sat in shader preparation for over 140 s with no log line.

**Cause.** The crash journal (0017) could not tell a kill from a crash. Every unclean end during the
startup phase counted as a startup crash, and that phase lasted until 300 frames after the
precache, so it reached into the first seconds of play. A "startup crash" with no pipeline in flight
deleted both caches: killed 1 s after a warm Ultra start, the next start compiled all 10076 pipelines
again, **297 s** (the log said "Startup crash: deleting ...", then nothing until the game started).
Killed while compiling, the configs in flight became crash suspects. They were left out of the
precache and compiled in play instead. One config in flight for two render target layouts was counted
twice, i.e. refused for good after a single kill. Not the cause: entries written before the kill were
all there (Dawn stores each shader and pipeline in its own committed WAL transaction as it is made;
pipeline_cache.db rows go through the writer thread as they come). There was no long WAL replay and
no lock held; the whole-file `quick_check` after an unclean exit (0226) costs seconds, not minutes,
on this PC, but could on a hard disk.

**Fix** (`patches/aurora/0231-crash-restart-keeps-caches.patch`, `src/port/crash_handler.cpp`,
`src/recomp/precache.cpp`):
- The crash handler (abort, std::terminate, unhandled exception, fatal signal) calls
  `aurora_pipeline_cache_note_crash()`, a flag in the mapped journal. An unclean end without it was
  a close or a kill: caches kept, no suspects ("ended ... without a crash report (closed or killed):
  the shader caches are kept; only shaders it had not finished are compiled").
- Real crashes: suspects are counted once per config and still precached (one that builds fine is
  lifted); the caches are reset only on the third startup crash in a row (crash-loop breaker,
  with the shipped list ignored as before).
- The startup phase ends 30 frames after the precache is done.
- `dawn_cache.db` gets no whole-file check any more, also after an unclean exit. Entries are
  verified by their checksums as they are read, and an SQLite corruption error on access resets the
  file. The background prefetch runs after an unclean exit too. `AURORA_CACHE_QUICK_CHECK=1` runs
  the check (testing).
- While held, a log line every 5 s: `[precache] preparing shaders: N pending, T s so far; this batch:
  N loaded from the cache, N compiled`. The screen says why it compiles: "The last session ended
  before all shaders were built (closed, killed or crashed): only the missing ones are compiled",
  or "crashed at startup several times in a row, the shader cache was cleared" after a reset.
- Test aid `LM_CRASH_AT=<s>`: abort() that long after startup (a real crash for the journal).

**Measured** (`LuigisMansionRecomp`, Enhanced lighting Ultra = 10076 pipelines, fresh cache):

| Sequence | Before | After |
|----|----|----|
| Warm start, killed 1 s after the game started, next start | caches deleted, **297 s** (all compiled) | 1.49 s, 0 compiled |
| Killed during a warm hold, next start | (startup crash) | 1.30 s, 0 compiled |
| Fresh: killed at 90 s while compiling, next start | 90 s to the game (11697 compiled, rest loaded); 8 in-flight configs left out of the precache, one refused for good | |
| Fresh: killed at 90 s, killed again 40 s into the next start, then a full start | | 10519 -> 16640 -> 20278 cache entries kept across the kills; last start 21 s (4228 compiled, 22604 loaded); all 10076 built every time, no suspects; the start after that 1.35 s |
| Crash (`LM_CRASH_AT=6`) while compiling a cache with 30% missing | | 7 suspects (once each), all precached and lifted; 36 s, only the missing 6115 compiled |

Also fixed on the way: a Windows user folder with characters outside ASCII (e.g. "José")
crashed at startup. `main.cpp` passed the ANSI code page string of `%APPDATA%` as Aurora's cache
and user path, which Aurora reads as UTF-8 (`std::filesystem::path` threw in the crash journal
check); both are UTF-8 now (`port::envPath` for `LM_CARD_DIR`/`LM_CACHE_DIR`).

### Hold frames below the refresh rate (issue #33)

**User report (public issue #33, i9-12900K, RTX 3070 Ti, Windows 11):** "Preparing shaders" crawls
on every start, warm cache too (under 10% after a minute); alt-tabbing away for a second and back,
it is done at once; started with `start "" /high` (HIGH priority class) it takes ~25 s. The owner
(also a 12900K) could not reproduce it.

**Cause.** The hold's frame loop (`main.cpp`, `precache::holdGame`) drew a frame, then slept 8 ms:
about 115 frames a second. On a display under ~115 Hz (the reporter's; 60 Hz is typical) that is
faster than the swap chain, so every frame the render worker waited in Dawn's `GetCurrentTexture`
for a free image until the next vertical blank. Dawn holds its **device lock** through that wait
(implicit device synchronization, Aurora 0175), and every pipeline creation takes it several times
(the three `PushErrorScope`/`PopErrorScope` pairs of `guarded_create`, the shader module, the
pipeline). The creation threads queued behind the acquire, frame after frame. A display at 120 Hz
or more (the owner's, this PC's) never filled the swap chain at 115 frames a second: no wait, no
slowdown. Alt-tab: the window lost the focus, Aurora paused (`SDL_WaitEvent`), no frames, so no
acquire, and the creation threads finished the batch in a second. The HIGH priority class only
shortened the lock hand-offs. Not the #18 thread policy: the same with `LM_CPU_QOS=0`.

**Measured** (this PC, 120 Hz; `LM_HOLD_FPS=0` lets the hold run at the refresh rate, which
reproduces the reporter's case; temporary timing in the render worker and `guarded_create`):

| | acquire (`GetCurrentTexture`) per frame | `PushErrorScope` x3 per pipeline | creation per pipeline | warm Ultra hold (10076) |
|---|---|---|---|---|
| hold at the refresh rate (`LM_HOLD_FPS=0`) | 6.0-6.5 ms | 3.2 ms | 11.9 ms | 8.1-8.7 s |
| hold below the refresh rate (115 fps here, 20 fps new) | 0.03 ms | | 1.7 ms | 1.7-2.2 s |

Warm starts, hold seconds, two runs each, other agents' game instances running (`LM_HOLD_FPS=old`
= the previous 8 ms sleep, not vsync-bound at 120 Hz):

| Settings | old (120 Hz) | at the refresh rate (= old on a 60 Hz display) | new | at the refresh rate, `LM_CPU_QOS=0` | new, `LM_CPU_QOS=0` |
|---|---|---|---|---|---|
| Default (3055) | 0.47-0.62 | 2.28-2.37 | 0.47-0.53 | | |
| Enhanced lighting Ultra (10076) | 1.67-2.14 | 8.14-8.20 | 1.81-1.93 | 8.10-8.33 | 1.96-2.14 |
| Ultra, visible window | | 8.68 | 2.23 | | |

Cold (fresh Dawn cache, driver cache warm) the lock matters little next to the compiles: default
42.8 s at the refresh rate, 40.0 s new.

**Fix** (`precache::holdFramePause`, after every hold frame and on the setup screens while the
game is prepared or its shaders compile): hold frames at most every 50 ms and never faster than half
the display's refresh rate (SDL's display mode; a wrong or unknown rate still gets the 50 ms), so a
swap chain image is always free and the acquire never waits. 20 frames a second are plenty for a
progress bar; a settings-change hold with the F1 menu open also runs at 20 while it lasts. Gameplay
frames are unchanged (frame pacing, #18). `LM_PIPELINE_LOG=1` logs `[precache] hold frames: N in 1 s
(every X ms, display Y Hz), aurora_begin_frame A ms avg`: a few ms every frame means the hold
frames run into the swap chain again. Test aid `LM_HOLD_FPS=<n>` (`0` unlimited = the refresh rate,
`old` = the 8 ms sleep).

Rule: any thread that needs Dawn while the render worker presents waits for the acquire too (FIFO
processor, pipeline workers); keep loops that present without a game frame below the refresh rate.

## Enhanced lighting

Optional effects layered on top of the accurate GX rendering: F1 > Visuals > Enhanced lighting
(`[enhanced_lighting]` in settings.ini). **Master switch off by default**; off, the renderer
emits nothing new (no draw-class commands, no scene-end marker, no shader variants), and frame
dumps are byte-identical to the plain renderer (nursery: master off vs the plain frames of an
`LM_ENHANCED_AB` run, and earlier against the main build).
Every part has its own switch and strength/quality control; the Quality preset
(Low/Medium/High/Ultra/Custom) sets soft-shadow, volumetric and colored-light-shadow quality.
Code: Aurora patch `0016-enhanced-lighting.patch` (`lib/gfx/enhanced.cpp/.hpp`,
`include/aurora/enhanced.h`, `include/aurora/lights.h`, shader generator changes in
`lib/gx/shader.cpp`), port side `src/recomp/enhanced_lighting.cpp/.h` (settings → Aurora,
draw classes, scene bracket, test lights), `shadows.cpp` (caster-pass class, shadow map size),
`unused_content.cpp` (ghost pass), `ghost_lights.cpp` (the ghost light provider, below).

### How it hooks in

- **Draw classes** (`GXSetDrawClassAurora`, FIFO command `GX_AURORA_SET_DRAW_CLASS`, so it is
  ordered with the draws and recorded in interpolation tapes) are set only at hook boundaries,
  never between the game's own GX commands: WORLD inside the main 3D view (`fn_8000BB98(0)`),
  GHOST inside its ghost pass (`fn_80060E70` pass 0x30), OTHER inside the shadow-map caster pass
  (`fn_8005D984` .. `fn_8005D8CC`) and everywhere else (HUD/2D, GBH, menus). Aurora additionally
  treats non-perspective draws as OTHER (2D quads inside the view: screen copies, ghost-layer
  composite). Only sent while the master switch is on. (A first version decided per draw in
  `hle_gx.cpp`'s draw preparation; inserting commands there between raw FIFO writes was fragile.)
- **Shader variants** (`ShaderConfig::enhClass`, chosen in `enhanced::shader_class`): 1 for
  WORLD, 2 for GHOST, 0 (the plain shader) for OTHER, for additive draws (light effects: the
  flashlight beam is built on GX vertex lighting and a color-texgen lookup), and for untextured
  lit draws (effect volumes whose color/alpha *is* the lighting result; the nursery's beam haze
  depends on one). Which parts run inside a variant is a uniform (`enh_flags`), so toggling a
  part never rebuilds pipelines; only the master switch does (one-time compile hitch).
- **Scene bracket**: `fn_8000BB98(0)` (main 3D view, US+PAL hook) is wrapped: before it the
  frame's extra lights are sent (`aurora::gfx::set_extra_lights`), after it `GXSceneEndAurora`
  marks the end of the 3D scene, before the HUD: bloom and the volumetric beam
  are composited onto the EFB there, so the HUD never glows and the CRT filter comes after.
- **A/B test aid** `LM_ENHANCED_AB=<n>`: game frames alternate in blocks of n between enhanced
  and plain (draws classed OTHER, no lights, no scene end: the plain renderer), so consecutive
  `LM_FRAME_DUMP` frames of one deterministic run compare both (with n = 1, odd dump frames are
  enhanced). The plain frames are byte-identical to a run with the master switch off.

### The parts

| Part (setting) | What | How |
|---|---|---|
| Per-pixel lighting (`per_pixel_lighting`) | The game's own GX lights evaluated per fragment with the same channel math (ambient/material sources, diffuse fn, spot/distance attenuation, clamps) | View-space position/normal interpolated to the fragment (`enh_pos`/`enh_nrm`; face normal from derivatives when the draw has no normals); WORLD draws only (ghost-pass draws keep vertex lighting: the ghost layer is composited from its own copy); channels that feed a color texgen keep their vertex result |
| Flashlight quality (`flashlight_quality`, softness, beam highlights) | Round beam pool with a smooth penumbra; glossy highlights where the beam hits | Spot color channels evaluated per pixel; below the cone's full strength the GX angular term is remapped through smoothstep (0 = GX curve, 1 = smooth). Alpha channels stay exact (the shadow light's ALPHA1 scales the shadow strength: smoothing it weakened shadows). "Beam highlights" (`flashlight_specular`, 0.6) adds a Blinn-Phong term (exponent 28) per spot light, tinted by the material color |
| Soft shadows (`soft_shadows`, quality, softness, contact hardening) | Soft edges for the game's shadow map (flashlight, lightning, room light) | The TEV `GR16_GT` compare stage (receiver key vs packed Z16 map) becomes a filtered compare: PCF 8/16 taps on a rotated Vogel disk, or PCSS (blocker search + penumbra from receiver-blocker distance) 16/32; receiver key computed analytically from the depth-ramp texcoord where it agrees with the TEV's key (no LINEAR byte-wrap stripes); capped slope-scaled bias on the off-centre taps only and a penumbra capped at 1.5x the radius (with a grazing shadow light, e.g. a ballroom ghost's own light near the floor, a full receiver-plane bias and wide penumbras erased thin casters' shadows); early out when the centre and 4 outer taps agree. Integer-TEV path included |
| Shadow look (`shadow_darkness` 0.9, `shadow_ambient_keep` 0.25: "Shadow darkness", "Colored shadow tint") | Shadows only remove the light they stand for: colored extra lights (ghost lights, test lights) keep lighting a shadowed pixel, a part of the ambient is kept, so shadows take on the surrounding color instead of going black | Aurora patch `0019-shadow-look.patch`: the game lerps a shadowed pixel toward the near-black K3 color by the shadow strength, whatever lights it. In the shader variants the shadow fraction from the compare stage is scaled by `enh_shadow_scale`: darkness × (lit − keep × ambient) / (lit + extra), with lit/ambient the luminance of color channel 0's GX lighting result and its ambient part, extra the extra lights reaching the pixel. 1.0 / 0 with no extra light = the game's darkening exactly; master switch off = untouched |
| Shadow map size (`shadow_map_size`) | 2048x1536 / 4096x3072 shadow map independent of the render resolution | `shadows.cpp`: the caster pass (`fn_8005D984` … `fn_8005D8CC`, new US+PAL hook) renders into an Aurora offscreen target (`GXCreateFrameBuffer`) of that size; the Z16 copy comes from it. Needs "Shadow map at render resolution" |
| Dynamic colored lights (`extra_lights`, intensity) | Up to 16 point lights per pixel on world geometry | `aurora/lights.h` interface below; added to the lit color channel (× material color) so the game's TEV decides the response; smooth (1-x²)² falloff to the radius, half-Lambert on ghost draws, optional specular |
| Colored light shadows (`extra_light_shadows`, method, lights, quality, map size, softness) | Lights flagged `ExtraLightCastShadows` (the strongest 2/4/8: ghosts, fires, candelabras, fire spray) are blocked by furniture/characters | **Shadow maps** (default, patch 0028; see "Colored-light shadow maps" below): cube maps per light in one atlas, PCF/PCSS lookup into the mask. **Screen space** (fast, up to 4 lights), current frame: the opaque world draws are re-rendered depth-only (same pipelines, depth layout) into a half-resolution pre-pass **before** the EFB pass that draws them (pass pre-op), a ray-march pass (8/16/32 steps, thickness 160 units) writes an RGBA8 mask (one channel per light), which is copied into a reserved storage-buffer range; the world shaders read it (`enh_mask`, bilinear) |
| Volumetric beam (`volumetric_beam`, intensity 2.0, quality) | Dusty haze in the flashlight beam, stopped by walls | Scene end: ray march along each view ray (16/24/48 steps, 1/4 or 1/2 resolution) through the flashlight's GX spot attenuation (the flashlight = the bright angular spot light nearest the camera among the world draws' lights), forward-scattering phase, depth-limited; density modulated by slowly drifting 3D value noise (dust motes) |
| Bloom / glow (`bloom`, intensity, light sources, bright surfaces, threshold, radius, flashlight-lit walls) | Light sources glow, the HUD never | Scene end, HDR (RGBA16F) chain at ≤ 720 lines: bright pass = **glow buffer** (emissive) + soft-knee scene highlights × "bright surfaces" (suppressed where the flashlight is the only reason a wall is bright); 6-level 13-tap downsample / tent upsample; composited additively onto the EFB |
| Ambient occlusion (`ssao`, intensity, radius; see "Ambient occlusion, horizon-based" below) | Contact darkening on solid surfaces | **Forward** (Aurora 0032): computed in the light-shadow mask pass from the opaque depth pre-pass (mask plane 4; since 0224 horizon-based from the pre-pass normals, before that 12 hemisphere samples) and multiplied inside the opaque world draws (depth-writing, not additive; `enh_flags` bit 16). Unlit textured opaque world draws get the enhanced variant while it is on, so toggling it rebuilds those pipelines once. The former scene-end multiply also darkened everything drawn over an occluded corner: the ghost layer, the flashlight beam and glare, glows, particles, glass (`AURORA_ENH_AO_POST=1` restores it for A/B) |

**Glow buffer (emissive-aware bloom).** Additive draws (blend SRCALPHA/ONE etc.: glows, lens
sprite, sparkles, fire, dust, lightning and nozzle effects; not the beam volume itself, whose
alpha is a color-texgen lookup: glowing it again washed the picture out when it points at the
camera) are recorded with a copy of their pipeline for a RGBA16F target and re-rendered right
after the EFB pass they were drawn in (before a later EFB copy could change textures), depth
tested (inclusive, no write) against that pass's depth, at bloom level-0 resolution. The copy
takes the draw's final state (Aurora merges consecutive draws into the recorded command later;
the first version re-rendered only the first fragment of the beam cone, a wedge pointing the
wrong way). The game draws the 3D scene, copies it (RGB565 640x480, color clear, depth kept),
draws the ghost layer and pastes the scene back before the HUD: glow draws survive that copy;
a color + depth clear (a new scene) drops them. Ordinary lit surfaces only glow through "bright
surfaces" (0.25 by default), so walls do not bloom; the glow buffer is Reinhard-compressed.
**Light glows** (removed in `enh-fixes-2`, Aurora 0187): each extra light used to add a small
screen-space halo at its position (core = 3% of its radius), hidden when the depth buffer showed
a surface in front of it. They showed as floating orbs (hidden ghosts, the light above Grabbing
Ghosts, fixtures); glow now only comes from pixels that are actually drawn.

### Extra light interface (`include/aurora/lights.h`)

```cpp
namespace aurora::gfx {
struct ExtraLight {
  float pos[3];       // VIEW space (GX eye space, same space as GXLoadLightObjImm positions)
  float color[3];     // linear RGB, may exceed 1
  float radius;       // world units; smooth falloff to 0 at radius
  float intensity;    // multiplier
  uint32_t flags;     // bit0: cast shadows, bit1: affects ghosts/translucent, bit2: specular
  uint32_t id;        // stable per-source id for interpolation pairing (0 = none)
};
void set_extra_lights(const ExtraLight* lights, uint32_t count);  // once per game frame
}
```

Port side: `recomp::submitExtraLights(lights, count)` (guest thread, once per game frame,
appending; up to 64 sent after culling and ranking, see "Light budget"); `enh::worldToView` converts world positions with the current
camera. The set is written into the GX FIFO (`GX_AURORA_SET_EXTRA_LIGHTS`) at the start of the
main view, so it is part of interpolation tapes: `interp.cpp` blends position, color, radius and
intensity between game frames by `id` (unpaired lights are shown as captured; jumps > 600 units
are not blended). "Translucent" (bit1) lets a light reach ghost-pass draws; ghost lights leave it
off. Test aid: `LM_TEST_LIGHTS=<count>[,<radius>[,<intensity>]]` (colored lights orbiting Luigi,
the first two cast shadows).

### Verification (US unless noted; `build/enhanced/*.png`; native 640x480 frame dumps of the
deterministic nursery script `LM_START_MAP=2 LM_START_APPEAR=1 LM_TEST_STATE=poltergust
LM_SKIP_CUTSCENES=1 LM_SKIP_AUTO=30 LM_INPUT_CLOCK=frames` with `LM_ENHANCED=1 LM_ENHANCED_AB=1`)

- Master off vs the AB run's plain frames: identical (5 frames); earlier against the main build.
- `nursery_beam_off_on.png`, `nursery_beam_crop_off_on.png`: flashlight on the door: dusty
  volumetric haze in the beam, smoother pool, lens glow.
- `nursery_lightning_off_on.png` (`LM_LIGHTNING=0,60,1`), `lightning_shadows_crop_plain_hard_soft.png`
  (plain / enhanced with soft shadows off = identical hard shadows / PCSS).
- `colored_light_shadows_off_on_mask.png` (`LM_TEST_LIGHTS=4`): colored pools on walls and floor,
  shadows of crib and bears, and the shadow mask (`AURORA_ENH_GLOWDBG=8`).
- Ballroom shadows (`LM_TELEPORT=1800,0,-1400,100`; the shadow light there is a dancing
  ghost's own light near the floor, so the long shadows change every frame with the dance):
  `ballroom_shadows_per_part.png` (enhanced frames of AB runs that are frame-aligned with each
  other, rows: everything off, per-pixel, flashlight quality, bloom, volumetric, soft shadows,
  default minus soft shadows) and `ballroom_shadows_alloff_default_ghostlights.png`: the game's
  shadows are kept in every configuration. The earlier report of missing shadows came from
  comparing different game frames (consecutive AB frames differ in the dance) plus two real
  issues fixed since: the penumbra smoothing was applied to the shadow light's ALPHA1 mask, and
  the soft-shadow bias/penumbra erased thin casters with a grazing light. Remaining visible
  difference: bloom brightens the white floor (bright surfaces 0.12 by default).
- `ghost_layer_ghostlight_ab_diff.png` (gold ghosts, room 42, ghost lights A/B with difference):
  changes only at the lit ghost and its light pool, no duplicate ghost shapes (the faint shapes
  in the ghost-lights agent's capture images are other ghosts at different times).
- Showcase: `ballroom_dark_showcase.png` (+ `_full_2560x1440`, `showcase2`, contact sheet):
  Ultra, ghost lights, bloom, ambient_scale 0.35, 16:9, render scale 3, window captures.
- `ws21x9_off_on.png` (21:9, render scale 2), `msaa4x_scale4_16x9_off_on.png` (MSAA 4x, scale 4,
  16:9), `pal_base_off_on.png` (European game via LuigisMansionRecomp setup),
  `hidden_mansion_off_on.png` (`LM_HM_FORCE=1`).
- Interpolation at 120 fps (`LM_INTERP_TEST=4`, walking, test lights): the in-between replays
  converge monotonically on the next game frame, the last step is identical to the captured
  frame; extra lights move smoothly (`interp_120fps_steps.png`).

### Cost

RTX 3090 Ti, nursery gameplay with 4 test lights, render scale 8 (5120x3840), uncapped at
320 fps target (the 120 Hz display caps at ~120), Ultra preset + AO + 4096 shadow map.
`LM_ENH_PERF_AB=6,<part>` switches one part off/on every 6 s within a run and `LM_FPS_LOG=1`
logs presented fps with the phase; numbers are the mean over ~18 two-second samples per phase
(other agents' game instances shared the GPU, so +-0.5 ms is noise):

| Part | Cost per frame at 5120x3840 |
|---|---|
| Everything (master switch) | +0.6 ms (after the fixes below; was +3.8 ms) |
| Per-pixel lighting | below noise (-0.3 ms) |
| Flashlight quality + highlights | +0.1 ms |
| Soft shadows (PCSS 32) | +0.45 ms (was +5 ms before the early out) |
| Colored lights (4) | +1.6 ms |
| Colored light shadows (2 lights, 32 steps) | below noise |
| Volumetric beam (High) | +0.05 ms (was +4.4 ms before capping its target at 720 lines) |
| Bloom | +0.6 ms |
| Ambient occlusion | +0.1 ms |
| Shadow map 4096x3072 | below noise |

At 1x-3x render scale every part is well below a millisecond. Turning the master switch on
compiles the enhanced shader variants once (a short hitch with the pipeline precache warm).

### Look

One shared look for colored light: "Light color vibrance" (`extra_light_saturation`, 1.6)
pushes every extra light's color away from its gray (keeping its brightest channel), for every
provider (all ghost lights, test lights) and their bloom halos; the flashlight, lamps, candles,
fireplaces, lightning and element effects get the same treatment through per-pixel lighting,
highlights and the emissive glow buffer. Each part keeps its own toggle and slider.

### Debug aids (Aurora, env)

`AURORA_ENH_LOG=1` (scene-end/flashlight/glow/light-shadow log), `AURORA_ENH_DRAWLOG=<n>`
(+`AURORA_ENH_DRAWLOG_ALL`: every draw of the n-th scene with class, blend, lighting, TEV),
`AURORA_ENH_GLOWDBG=1` (show the glow buffer), `=2` (one glow draw per frame), `=3/5/6` (all
world draws into the glow buffer / without depth test / inverted test), `=7` (glow pass depth),
`=8` (light-shadow mask), `=9/10` (mask pass depth / light distance), `AURORA_ENH_HIDE_ADDITIVE=1`
(drop additive world draws), `AURORA_ENH_NOVARIANT=1` (no shader variants),
`AURORA_SHADER_DUMP=<dir>` (every generated WGSL shader). Port: `LM_ENHANCED=0|1`,
`LM_ENHANCED_AB=<n>`, `LM_TEST_LIGHTS`, `LM_SHADOW_MAP=0|1|2`, `LM_LIGHT_TRACE=1` (each distinct
`GXLoadLightObjImm` call with its guest chain).

`LM_SETTINGS_AT=<frame>:<section>.<key>=<value>[,...][;<frame>:...]` changes settings (ini names)
live at a dump frame through the F1 path (`port::setSetting`, then the per-frame apply), e.g.
`2640:enhanced_lighting.shadow_darkness=0.2;2652:enhanced_lighting.shadow_darkness=0.9`. Two
concurrently started runs of the nursery script (`LM_ROOMS_LIT=1`, `LM_TEST_LIGHTS=4,450,1.5,0`,
preset Custom) are frame-identical, so a run with changes against one without isolates each setting.
Used to verify the 0031 fix: before it, Aurora's `same()` compared only the 0016 config fields, so a
change of only the colored-light shadow method/quality/map size/softness, shadow darkness or
colored shadow tint never reached the renderer (the shadow on/off and light count still worked,
through the port's light flags). After it every shadow setting changes the picture on the next frame
and reverting restores it exactly. 0031 also takes the glow depth from the farthest covered pixel
(re-rendered emissive surfaces on grazing floors) and the AO bias relative to the radius (from SMS
0150, docs/BACKPORT.md rows 77-80 there).

### Ghost lights

F1 > Visuals > Enhanced lighting > "Ghost lights" (`[ghost_lights]` in settings.ini; needs the
Enhanced lighting master switch and "Dynamic colored lights"; off by default). Every ghost the
game draws becomes a coloured point light in the renderer's extra-light list
(`aurora/lights.h`), so ghosts light the floor, walls and furniture around them in their own
glow colour. Code: `src/recomp/ghost_lights.cpp` (US + PAL).

**Where the data comes from** (USA addresses; `VA()` / `VOFF()` for the European game):

| What | Source |
|---|---|
| Ghost actors | live character slots `lbl_803D48A0` (count `lbl_804D8750`), the list the actor draw pass `fn_80060E70` walks. A ghost = type (actor +0x3C) with type flag 8 in `lbl_8033B590` (0x1C per type: +0x08 u16 flags, +0x14 s16 class 1 small / 2 normal / 3 portrait / 4 boss, +0x18 name) |
| Drawn this frame | the draw pass's own test: actor +0x40 flags (0x6000 not drawn, 0x100 hidden from the normal passes, 0x200 any room) and room byte (+0xB4 >> 24) visible through `fn_8001CE1C(room, -1)` |
| Position | actor +0x44, lifted 40/90/110/160 units (class 1/2/3/4): +0x44 is where the game puts the small GX light some ghosts carry (`EnZako::fn_800C1FFC` → `fn_80016C98(0, actor+0x44, BrightColor × alpha, 40, 80)`), but walking ghosts have it near the floor (ballroom shy-guy ghosts at y 60), where a point light only grazes the floor |
| Colour | the ghost's glow colour, EnemyParam "BrightColor" (`/param/ctp/<type>.prm`, `Koga::EnTypesManager::sManager`+4, 0x218 per type, value at +0x1FC; same layout on PAL), which the game copies to actor +0xE4 and uses for the ghost's light effects. The actor copy is read; parameter and a per-class table are fallbacks |
| Element ghosts | EnemyParam Element (+0x198): 1 fire, 2 water, 3 ice (from the weakness test at 0x800C75F8). They all share the pale glow C1DCF7, so their light is fire orange / water blue / ice cyan instead |
| Visibility | actor +0xE3, the alpha the ghost layer is drawn with (0 invisible .. 255) |
| HP | actor +0xFC (s16), maximum = EnemyParam life (+0x1C) |
| States | `Koga::CharacterEvent` messages to the ghost's AI (`fn_800C0F08`, hooked in gameplay_hooks.cpp; message → observer slot from `receiveMessage`'s jump table): 2 in the flashlight beam (every frame), 3 capture begin, 4 capture end (also sent when a ghost vanishes/escapes), 9 fishing begin (hooked by the Poltergust), 12 collided with Luigi, 13/14/15 fire/ice/water spray, 16/17/18 fire/ice/water ball. The Poltergust capture update `fn_80081BCC` (ghost at capture +0x18) runs every frame a ghost is held |
| Camera | GX view matrix `lbl_803A3820`+0x18C (+0x198 on PAL), the matrix `fn_80057CFC` uses to put the S3D lights in eye space for `GXLoadLightObjImm`; identical to `enh::worldToView` (checked) |

Per-type glow colours (US, `LM_GHOST_LIGHT_LOG` dumps the table): gold ghost `mgyapoo1`
F19726, Chauncey `bbaby` CCFF99, Boolossus/Boos `btelesa`/`telesa` white, Mr. Luggs `eater`
CC74F0, waiters A3DDE0, ballroom shy-guy ghosts `heypo1..8` red/green/white/brown/orange/
yellow/pink/purple, blue/ice ghosts `iyapoo*` 5ACBE1, gold mice F5C50F, Bogmire `bshadow`
white, Neville `situji` 0032FF.

**Behaviour** ("Dramatic ghost light effects", on by default): gentle two-frequency pulse; a
flare when a ghost fades in or collides with Luigi; a nervous flicker that brightens while the
flashlight beam is on it; while the Poltergust holds it the light ramps with lost HP
(0.35 + 0.65 × (1 − HP/max)) with a fast tremor and a larger radius; capture (message 3, or HP
reaching 0 while held) gives a ~3× burst that fades over 1.5 s and follows the ghost into the
nozzle (and stays at its last position if the actor leaves the list); element hits tint the
light with the element for 20 frames. Invisible ghosts give no light (the former "Invisible ghost glow"
hint is gone; since `enh-fixes-2` only ghosts the ghost pass actually drew visibly give light). Lights are sorted by strength / distance; "Max ghost lights"
(default 8, renderer limit 16) keeps the strongest, the strongest two get the cast-shadows flag
when "Shadows from ghost lights" is on (now as many as "Shadow-casting lights" allows; see
"Colored-light shadow maps"). Radius by class 380/560/720/1100 × "Ghost light radius"; ids = actor pointer, so
interpolation pairs them. Actors are pooled and reused, so capture marks expire.

The lights do not carry `ExtraLightTranslucent`: only draws of the ghost pass (`fn_80060E70`
pass 0x30) are ghost draws for the renderer, blended room geometry (much of the ballroom floor
and walls) is ordinary world geometry and gets the light, and a ghost's body does not light
itself; its halo comes from the game's own glow draws (through the glow buffer).

Built at the start of the main view `fn_8000BB98(0)` (enhanced_lighting.cpp's hook calls
`ghostlights::onMainView` before `enh::beginScene`) and submitted with
`recomp::submitExtraLights`; frames without the game scene have none.

**Verification** (US unless noted; build/ghostlights/*.png): `LM_GHOST_LIGHT_AB=1` sends the
lights only on every other game frame so consecutive `LM_FRAME_DUMP` frames compare with/without
in the same scene (frame brightness then alternates by ~7.5e6 per frame in the ballroom).
Ballroom (`LM_TELEPORT=1800,0,-1400,100`): coloured pools around the shy-guy ghosts on the floor
and the chandelier (`ballroom_ab.png`); Chauncey (`LM_START_MAP=10`): green light on the
balcony floor and balustrade (`chauncey_ab.png`); Boolossus (map 11): white light, collide
events flare it (`boolossus_ab.png`). Logged in game: gold ghost in the training room
(`LM_START_MAP=3`; appear flare, in-beam events, flee/despawn with message 4), Mr. Luggs and the
waiters in the dining room, the ballroom ghosts. PAL base (map 11): same ghost, colour, view
positions and events.

**Stun → vacuum → capture, in play** (gold ghosts, map 2 room 42 = enemyinfo room 39,
`LM_TELEPORT=0,550,-3300,100`; the ghosts spawn as soon as Luigi is in the room). What the
game does (US): the gold ghost strategy (`lbl_8035B6E4`) cycles AI states 4 appear → 5 approach
→ 8 hover in front of Luigi (~50 frames) → 0xA attack (fully visible) → vanish → 4 (9 is a
later hover). Its beam handler (`fn_800C7824`) only reacts in states 5/8/9 (`fn_800C8F88`): it
starts the light-bind sub-strategy at strategy +0x504 (`fn_800C8FE8`, pointer at +0x590), which
counts frames the ghost is still lit (message 21 = EnThought +0x908 > 0; message 2 sets +0x908)
against `fn_800DBE8C`: the type's LightBindFrame (gold ghost 25) scaled by distance between the
Min/MaxLightBindRange 100..400 (closer = more frames). In state 5 it also starts fleeing.
Sub-state 3 = stunned: heart and HP counter shown, and it fades away within ~15 frames unless
the vacuum hooks it.

Why the first attempts never vacuumed: each time a ghost appears Luigi is startled for ~77
frames (Poltergust mode, Luigi +0x1180, = 0; `LM_NOZZLE_DUMP=1`), and that window covers the
first hover/attack. A stun inside it ends before R can do anything. R itself works under
`LM_TEST_STATE` once gameplay has started. The hunting aid therefore stuns only while the mode
is non-zero and holds R from the moment it aims (`LM_GHOST_HUNT_R=0`), nozzle tilted up (C stick)
and pulling away once hooked.

Run that completes the whole sequence (deterministic): `LM_GHOST_HUNT=9 LM_GHOST_HUNT_BEAM=89
LM_GHOST_HUNT_DMAX=300 LM_GHOST_HUNT_R=0` plus the teleport above, `LM_TEST_STATE=poltergust`,
`--input "600:START*10,1000:A*6"`: aim at f2546 (state 9, 216 units), R → Poltergust mode 1,
message 9 (hooked) at f2556, HP 10 → 0 by f2626 (capture update holding the ghost every frame),
message 3 (capture begin) f2634, message 4 (inside the Poltergust) f2662. Ghost light intensity
over it (`LM_GHOST_LIGHT_LOG`): in the beam ~2 (flicker), held 3-7 rising with lost HP plus
tremor, capture burst peaking at ~6 at f2629 and fading to 0.3 by f2659, off once captured.
The beam stun is the stand-in (`LM_GHOST_HUNT_BEAM` delivers the game's own message 2 in
states 8/9; aiming the real flashlight up at the hovering ghost from a script did not work);
everything after it is the game's own vacuum and capture.

Frame dumps with `LM_GHOST_LIGHT_AB=1` (off / on / difference; presents 4990..5101 of that run):
- `gold_ghost_stun_ab.png`: lit and stunned (heart shown): orange light on the wall and floor.
- `gold_ghost_vacuum_ab.png`: hooked, HP 10 → 4 → 2: the pool on the floor and walls grows
  brighter and wider as the HP drains.
- `gold_ghost_capture_ab.png`: capture burst (the room lit orange), fading while the ghost is
  pulled into the nozzle, nothing once it is inside.
The first capture peaked at ~17× (burst multiplied by the vacuum struggle) and washed the room
out; the struggle is no longer applied during the capture.

Element ghosts and element hits have not been exercised in play.

Test aids: `LM_GHOST_LIGHTS=1` (on regardless of the settings; with `LM_ENHANCED=1`),
`LM_GHOST_LIGHT_LOG=1|N` (every ghost each N frames: id, type, class, element, state, alpha,
HP, flags, colour, world and view position, intensity, radius; the lights sent; every event;
vacuum holds; the type colour table once), `LM_GHOST_LIGHT_DIFF=1` (changed bytes 0..0x200 of
drawn ghosts, research aid), `LM_GHOST_LIGHT_AB=1`, and the hunting aids (US, research):
`LM_GHOST_HUNT=1|<AI state hex>` steers Luigi (after `--input`: faces away from the target
ghost while it hovers, turns to it in the given state, holds R and closes in with the nozzle
tilted up once it is stunned, pulls away when hooked), `LM_GHOST_HUNT_BEAM=<states>`,
`LM_GHOST_HUNT_DMAX`, `LM_GHOST_HUNT_DELAY`, `LM_GHOST_HUNT_R`, `LM_GHOST_HUNT_TURN`,
`LM_GHOST_HUNT_CY`, `LM_GHOST_HUNT_DIV`, `LM_GHOST_HUNT_FLIP`. The log line's `ai` field is the
ghost's strategy state (EnThought +0x800 → +0x0E; decorator inner state in the high half), and
the first AI of each type logs its strategy vtable.


### Light budget (Aurora patch 0023)

`MaxExtraLights` 16 → 64 (the fragment loop's bound follows; the bloom glow cores stay the
16 strongest, `fx.xl` holds 16 pairs). `recomp::submitExtraLights` now appends (ghost lights,
world lights, test lights); at the start of the main view the candidates are culled (sphere
entirely behind the camera or outside a generous ~136-degree view cone) and ranked by
intensity x radius over distance, and the strongest 64 are sent in that order (Aurora gives the
four shadow slots to the first flagged ones). The per-pixel loop rejects a light with one
distance test, so per-tile light lists were not needed at these counts. Ghost lights: "Max
ghost lights" now 1..64, default 48 (setting key `max_lights_64`, so the old default of 8 does
not carry over). Small ghosts (class 1: bats, mice, Boos, rats) are in the ghost list like the
rest (type flag 8) and glow at 0.55x (several hang right under hallway ceilings, where full
strength burnt hot spots into the plaster).

Verified in the 1F west hallway (map 2 room 6, `LM_TELEPORT=-1600,0,-985,100`; the generator
table has bat1 x2 + bat2 x4 there): all six bats tracked and sent as lights every frame
(`LM_GHOST_LIGHT_LOG`: `light 0..5`), their glow on the ceiling corner
(`build/worldlights/hallway_bats.png`).

### Character material (Aurora patch 0024)

User report: Luigi looked shiny and a bit odd under enhanced lighting (and his face a bit dark in
the beam). Cause: characters went through the world path, so the per-pixel re-evaluation of the
game's GX lights (with its flashlight highlight, `pow(n.h, 28)`) and the extra lights' specular
lobe made cloth and skin glossy, and the per-pixel spot evaluation on his low-poly normals
darkened the face.
- New FIFO command `GX_AURORA_SET_MATERIAL_HINT` (`GXSetMaterialHintAurora`,
  `AURORA_MATERIAL_WORLD` / `_CHARACTER`): a uniform only (the free `enh_material` slot of the
  draw uniform), so no new pipeline variants and nothing new for the precache.
- Port: `enh::setCharacterPass` around the actor draw pass `fn_80060E70` (every pass but the ghost
  layer) tags Luigi, NPCs and other actors as characters (the hook in unused_content.cpp).
- Characters keep the game's own vertex lighting (per-pixel bits 0/3 cleared for their draws:
  the original balance, no flashlight highlight). The colored extra lights still reach them, with
  a matte wrap-around response (`(n.l + 0.45) / 1.45 x 0.85`) and no specular, so ghost, candle,
  fire-spray and lightning light colors Luigi softly without hot spots.
- Test aid `LM_CHAR_MATERIAL=0` (off) / `2` (even frames only, for A/B dumps).
- `build/worldlights/luigi_material_before_after.png`: nursery, left before (glossy highlight on
  the cap), right after (matte, the game's shading); `fire_spray_ab.png` shows Luigi taking the
  spray's orange light.

### World lights

F1 > Visuals > Enhanced lighting > "World lights" (`[world_lights]`; needs the master switch and
"Dynamic colored lights"; on by default inside the enhanced mode, the vanilla renderer is
untouched). Per-category toggles and intensity sliders (Candles, Fireplaces and torches, Lamps
and chandeliers, Element sprays, Fire effects, Lightning, Sparkles and glows), a master
intensity, "Flame flicker" and "Merge nearby flames"; colors go through the shared "Light color
vibrance". Code: `src/recomp/world_lights.cpp` (hooks tagged US + PAL).

**Sources.**
- Flames and glows are the game's own particle emitters. The LPS manager `lbl_803D990C` (drawn
  by `fn_801604FC`) keeps live emitters in a list: manager +0xAB4 group, group +0x2A0 sentinel,
  emitter +0x20C next; emitter +0xBE flags (8 drawn, 2 emitting, 0x40 room not visible, set by
  the draw's room test `fn_8015F990`), +0x214 world position. Resource names are recorded in a
  hook on `JKRArchive::getResource(type, name)` (.pcr data pointer → name) and emitters are
  matched to their resource in a hook on the creation `fn_8015FE9C(manager, pcr, texture, ...)`.
  The map's `objinfo` places the flame objects (`fire`, `sfire`, 122 `chandelier`); the mansion's
  candles exist as emitters all the time and give light only where the game draws them, emitting,
  in a visible room.
- Kinds by resource prefix (`kKinds`): `candle_*` (orange; `candle_vio` violet), `danro_fire`
  (hearths and braziers), `torch` / `tc_s*`, `chandelier` / `shanyb` / `m_light1_*` (lamps),
  `fireball` / `firehit` / `kpa_fire` / `lg_eledmg_f` / `kobk_el_fire` / `kaen_hinoko` /
  `iceball` (fire effects), `kira_*` / `coin_ef` / `key_kira` / `bkey_*` / `btel_*` /
  `dancer_kira` / `takarabako` (subtle sparkles). Nearby emitters of one kind merge (a
  candelabra = one light at the centroid, intensity x n^0.6, radius +15% per extra flame); id =
  the lowest emitter address, stable while the emitter lives.
- Flicker: three octaves of smooth value noise per light; amount per kind (candles 0.8, fires
  1.0, lamps 0.1-0.15, sparkles 0.3-0.5). Updated per game frame; interpolation blends by id.
- Poltergust element spray: the spray is a model (`elefire.bmd`), not particles. Luigi's
  Poltergust mode (+0x1180: 2 fire, 3 water, 4 ice while L is held) and the nozzle actor
  (+0x7A4, position +0x44) place the light 170 units ahead of the nozzle; it ramps in and out
  over a few frames; fire 2.2 orange with flicker, water and ice 0.7 blue / cyan.
- Lightning: the strike `fn_80029250` (hooked) points the game's shadow light camera at the
  window (camera +0x24); a cool blue-white light 700 units from Luigi toward it flashes
  (1 / 0.35 / 0.9, then decays over ~1.5 s) on top of the game's own flash. (The thunder
  entry's +4/+8/+C are the light's fov/near/far, not a position.)

**Open.** Lamps and chandeliers light only through their particle emitters (`chandelier.pcr`
etc.), which appear in lit rooms; no lit room was reached in testing, so that path is
unverified. Next: a fallback from the map's `objinfo` chandelier objects with the room's own
`roominfo` LightColorR/G/B (warm/cool per room). The GBH screen and mirror glows are not light
sources yet; fire ghosts get their element color from the ghost lights. One transient Dawn
crash (bind group creation) happened once during a frame-dump run and did not reproduce.

**Verification** (`build/worldlights/*.png`; A/B via `LM_WORLD_LIGHT_AB=1`: lights only on even
game frames, so consecutive frame dumps compare with and without):
- `kitchen_fire_ab.png`: the brazier on the pillar (`danro_fire`) lights the wall and floor.
- Foyer: the three-candle groups on the side tables and the upstairs candles (6 lights in
  `LM_WORLD_LIGHT_LOG`), warm pools on the side walls (`foyer_candles_ab.png`).
- `fire_spray_ab.png`: nursery, fire spray (`LM_TEST_STATE=poltergust+fire`, L held): walls, crib
  and Luigi take a flickering orange light.
- Lightning (`LM_LIGHTNING=200,150`, nursery): strike hooked, the flash placed toward the window
  (`lightning flash at ...` in the log); the game's own frame-to-frame flash flicker dominates
  the A/B dumps, so there is no clean image.
- 120 fps (`target_fps=120`): interpolation active (`[interp] ... blended`), lights blended by id.
- Enhanced off: world lights are not built and no material hints are sent (both need
  `enh::masterOn()`); patches 0023/0024 only change the enhanced paths, so the plain renderer is
  unchanged by construction.

Test aids: `LM_WORLD_LIGHTS=1` (on regardless of the setting), `LM_WORLD_LIGHT_LOG=N` (every
emitter with name/flags/position, the lights sent, spray and lightning state, Luigi/nozzle
orientation), `LM_WORLD_LIGHT_AB=1`.


### World lights, second pass

**Fire color (Aurora patch 0025).** The fireplace/brazier read as saturated pure red. Cause: the
shared "Light color vibrance" (`vivid()`, default 1.6) pushed each channel away from the
luminance and clamped the ones that went negative; for orange (linear 1, 0.33, 0.05) blue and
then most of green were clamped, a hue shift toward red. Now the boost stops where the weakest
channel reaches 0 (hue-preserving), and flames carry `ExtraLightFire` (bit 3): in the shader
green and blue fall off faster than red with distance (`col * (1, mix(1.2, 0.62, x), mix(1.5,
0.35, x))`, x = distance / radius), so a fire is yellow-white near the flame and red-orange at
the edge of its reach. Set on candles, fires, fire effects (warm colors only) and the fire spray.

**Lit rooms: lamps and chandeliers.** A cleared room's light is on when its room object
(`lbl_804D8178`, 0x14C per room) has +6 bit 0x80, the test `fn_80018D78` (used by the
`CondLightOn` spawn condition). Test aid `LM_ROOM_LIGHTS_ON=1` sets it for the room Luigi is in
(the game then draws the room lit, chandelier glow included). Light positions come from the map
archive read from the disc (the game loads its jmp tables through the tool-data loader, not by
name): `jmp/objinfo` "chandelier" objects (122 in map 2, 40 units below the object) and
`jmp/furnitureinfo` light fittings by model (`syan*`, `o_syan*`, `osyan*`, `shan*`: chandeliers 60
below; `a_lamp*`, `lamp*`, `a_ligh*`, `a_lump*`: lamps 90 above); each is assigned to the room
containing it by the room table bounds and lights while that room is lit and visible, in the
room's own color (`jmp/roominfo` LightColorR/G/B, e.g. ballroom FF8C6E), chandeliers 950 units
reach, lamps 520. Map 2: 201 fittings, 72 room colors. In an already lit room the extra warmth is
subtle (the room is bright), most visible on the floor under the chandeliers.

**Game Boy Horror.** While Luigi holds it up (the GBH actor at Luigi +0x7AC, type 0x0A, drawn),
a soft green screen glow (230 units). Mirrors are not light sources (they reflect; the game has
no glow for them).

**Cost of 64 lights** (after the thread-leak fix; RTX 3090 Ti, nursery, render scale 8 =
5120x3840, uncapped, `LM_ENH_PERF_AB=6,xl` + `LM_FPS_LOG=1`, 12 two-second samples per phase,
GPU shared with other agents): worst case, all lights overlapping Luigi on screen
(`LM_TEST_LIGHTS=n,450,1.5`): 64 lights 56.0 fps vs 113.9 fps without colored lights (+9.1 ms per
frame), 16 lights 102.5 vs 112.1 (+0.9 ms), 4 lights below noise. Cost scales with pixels x
overlapping lights: at render scale 3 (2560x1440, 1/7 of the pixels) the 64-light worst case is
~1.3 ms; real rooms have far fewer lights overlapping any pixel (a candle reaches 360 units). If
it ever matters, per-tile light lists would cut the overlapping case.

**Room tour** (lights from the log, `LM_WORLD_LIGHT_LOG`): flame objects per room from objinfo
(`fire`/`sfire`/`chandelier`) show where candles are; e.g. the 2F room at x 1575..3125, z
-3860..-2810 has 30 candles, the 1F room east of the foyer 12 table candles. Showcase captures
below.

**Showcase captures** (window grabs, 2560x1440, 16:9, render scale 3, Ultra preset, ghost and world
lights on; `build/worldlights/showcase_*.png` plus `_720p` copies): `dining_brazier` (the fire
bowl on the pillar and the violet candles), `foyer` (side-table candelabras), `dining_candelabras_ghost`
(Mr. Luggs' glow and the violet table candles), `fireplace` (Luigi lit by the fireplace),
`fire_spray` (nursery, fire spray in the dark), `hallway_bats` (the bats' glow under the ceiling),
`lit_ballroom_chandeliers` (`LM_ROOM_LIGHTS_ON=1`), `ballroom_ghosts` (the shy-guy ghosts' colored
glow around Luigi).

### World lights, third pass

**Vibrance at full saturation (Aurora patch 0026).** 0025 stopped the boost where the weakest
channel reached 0. That kept hues but left already saturated ghost colors (the shy guys' red,
Chauncey's green, the gold ghost's orange) less punchy than before. `vivid()` now works on HSV
saturation: the boost scales saturation by the vibrance and may push it up to 1 (fully
saturated) while hue and value stay put, so strong colors get as vivid as the old look without
the old clamp's shift toward red. Debug compare: `AURORA_VIVID_MODE=0` (old per-channel clamp),
`1` (0025 early stop), `2` (default, full saturation). Side by side (old | 0025 | 0026):
`build/worldlights/vibrance_{shyguys,chauncey,goldghost}_old_0025_0026.png`.

**GBH glow only when raised.** The GBH actor hangs on Luigi's belt most of the time (67-89 units
above his feet), which lit his legs green constantly. The glow now needs the GBH more than 105
units above Luigi (held up to his face, E. Gadd's calls). Not captured yet: holding X/Y/Z/B in a
hallway never raised it (log `gbh <n> above Luigi` stays below 90); only the call cutscenes do.

**Room tour, rest of the mansion** (window grabs, same settings as below):
- Parlor (room 1, lit): sconces and chandelier warm the walls; `showcase_parlor_lit`.
- Fortune-Teller's room (1F, Madame Clairvoya's): her candles exist as emitters but without the
  emitting flag (0x000C) until her event, so they give no light; the crystal-ball room is dark
  apart from the flashlight before she appears (`showcase_fortune_teller`). Her own glow is a
  ghost light. The 2F 30-candle room is the Astral Hall (room 43): candelabras unlit before its
  event, no light.
- Graveyard (map 2 outdoors): both torches (`danro_fire` emitters) light the steps and ground
  orange; `showcase_graveyard_torches`.
- Boneyard: Spooky's doghouse glow (ghost light, blue); `showcase_boneyard_spooky`.
- Billiards room: the cyan ghost's light on the wall and table; `showcase_billiards_ghost`.
- Kitchen, Conservatory, Twins' room: no colored source in the captured views (ghosts not yet
  present), flashlight only.
- Washroom with Toad: no colored source nearby; Toad goes through the character material path
  like Luigi and is lit by the flashlight (`showcase_washroom_toad`). E. Gadd's lab, the portrait
  ghosts' portraits and Mario's painting were not captured in this pass.

**Hallway bats visible.** Standing near the west end of the 1F west hallway
(`LM_TELEPORT=-2050,0,-900,100`) puts the hanging bats in frame: their yellow bodies (the game's
own look, unchanged with lights off) and the glow on the ceiling corner, with a purple ghost
passing in the same shot; `showcase_hallway_bats_visible`.

### Colored-light shadow maps (Aurora patch 0028)

The screen-space light shadows (patch 0016/0020) only know what the camera sees: a blocker
outside the picture (behind the camera, beside it, above the frame) casts nothing, a ray toward a
light behind the camera leaves the screen at once, and the fixed "thickness" turns thin objects
into long smears. Shadow maps fix all three. F1 > Visuals > Enhanced lighting: "Light shadow
method" (Shadow maps, default / Screen-space), "Shadow-casting lights" (Off/2/4/8), "Light shadow
quality", "Shadow map size" (256..2048 texels per cube face), "Light shadow softness" (light size
in world units, default 14). Presets: Low 2 lights at 256, Medium 2 at 512, High 4 at 512, Ultra
8 at 1024. Settings keys `[enhanced_lighting] extra_shadow_mode / extra_shadow_lights /
extra_shadow_map_size / extra_shadow_softness`.

**Which lights.** Providers flag `ExtraLightCastShadows`: ghost lights (the strongest ghosts, as
many as the setting allows), fires (fireplaces, braziers, torches), candelabras (three or more
candles merged into one light), big fire effects and the fire spray. `selectLights`
(enhanced_lighting.cpp) ranks every light (intensity x reach over distance); the first N flagged
lights in that order get a shadow slot in Aurora. Each light's shadow fades in/out over a few
frames when it gains/loses a slot (weight in flags bits 8..15, multiplied with the ghost lights'
own fade), so the strongest set changing as Luigi walks does not pop.

**How (one frame).**
1. Every opaque world draw that the depth pre-pass already re-renders also gets a *caster*
   pipeline variant: `ShaderConfig::lightDepth` (a former padding bit, so existing configs and
   the shipped pipeline list keep their keys), depth only, no culling, depth test LEQUAL, no
   polygon offset. Its vertex shader takes the draw's own eye-space position (`mv_pos`) and
   multiplies it by a matrix read from the storage buffer at `imm._pad + instance_index * 20`;
   the fragment shader discards fragments outside the record's tile and keeps the game's alpha
   test (fences, plants, cut-out cards cast cut-out shadows). The enhanced lighting code of the
   variant is off (the uniform block keeps its layout, so the original draw's uniforms are used
   as they are).
2. Before the EFB pass whose world draws read the mask (the same pre-pass op as before), the
   caster draws are rendered into a depth atlas: per shadowed light a cube map as 3x2 faces of
   S x S texels, lights in two columns (8 lights at 1024: 6144 x 8192). One instanced draw per
   caster covers every light and face (instance = light x 6 + face): about 85 draws in a room,
   not 85 x 48. A caster whose pipeline is still compiling is skipped (`try_get_pipeline`)
   instead of stalling the frame; its shadow appears a frame later. The pipelines are recorded
   in the pipeline cache like any other, so later sessions precompile them.
3. Face matrices: view space -> face (forward/right/up along the view axes) -> perspective with
   a field of view a little over 90 degrees (tan = 1 + 48/S, so a filter kernel near a face edge
   stays on that face) -> the face's tile in atlas clip space. Depth is reversed (z = near/axis
   distance, cleared to 0), near = 2% of the light's reach (3..20 units), so the light's own
   holder (candle stick, brazier bowl) does not wrap it in shadow.
4. The mask pass (`fs_xlcube`, two RGBA8 targets = 8 lights) reconstructs each pixel's view
   position and normal from the camera depth pre-pass, picks the cube face by the major axis,
   offsets the point along the normal (more at grazing angles) and toward the light by about one
   texel, and filters: PCSS blocker search (8..10 taps) in a region the size of the light, umbra
   early-out when every search tap is blocked, then 8/12/20 PCF taps (quality) with hardware 2x2
   comparison each. Taps lie on the receiver's tangent plane in world space and are projected
   one by one (receiver-plane depth), so a wide penumbra on a floor lit at a grazing angle does
   not shadow the floor itself. The per-pixel tap rotation is fixed (no crawling between frames
   or interpolated replays). Pixels on a one-pixel depth ledge (both neighbours on an axis jump
   away) use the light direction instead of the reconstructed normal for the offset.
5. The mask now lives in its own GPU buffer (`maskBuffer`, 32 MiB, static bind group binding 2,
   `mbuf` in the GX shaders) instead of a range of the per-frame storage upload: nothing is
   uploaded for it any more (before: 3.7 MB of zeros per frame at 1280x720), and it can be full
   resolution: shadow maps at high quality (Ultra) use a full-resolution mask (up to 1440 lines),
   otherwise half resolution (the receivers' 4-tap tent smooths it). Two planes: slots 0-3 and
   4-7 (`enh_mask` reads plane slot/4).
6. Casters and the depth pre-pass use a depth-only shader variant (`ShaderConfig::depthOnly`,
   also a former padding bit): the copy no longer runs the enhanced per-pixel lighting it cannot
   show (pre-pass 1.3 -> 0.2 ms in a furnished room). Each instance is clipped to its face with
   `clip_distances` (Dawn feature ClipDistances; without it the fragment shader discards outside
   the tile, which is much slower: 13 ms instead of 1.5 ms for 8 lights at 1024). A second mask
   in the same frame (a later EFB pass) only adds the new draws to the pre-pass and the atlas
   (load instead of clear) instead of rendering every caster again.

**Frame interpolation.** Nothing here is cached across presented frames: tape replays go through
the command processor like a fresh frame, so the caster list, the light positions (blended by
id) and the atlas are rebuilt from the replayed (blended) draws every presented frame, and the
shadow always matches the geometry it is drawn with. Cost therefore scales with the presented
frame rate, like the other enhanced passes. The hooks are all inside Aurora's enhanced lighting
(`note_draw` / `flush_shadows` / `encode_mask`) and the shader generator; no interpolation code
changed. Overlap with the interp-everything branch: none in `src/recomp/interp*.cpp`; if that
branch changes how replayed draws reach `note_draw` (e.g. skipping the command processor), the
caster list must still be filled from the replayed draws.

**Limits.** Only what the game draws casts: the game culls rooms and actors outside the view
(portal culling, actor visibility), so a blocker in a room the game does not draw this frame
casts nothing. Ghosts (ghost pass) and additive effects do not cast. Translucent receivers
(ghosts lit by translucent lights) use the depth behind them. 8 lights at 2048 are reduced to
fit the 8192 texture limit.

**Test aids.** `LM_XL_SHADOWS=<mode>[,<lights>[,<face size>]]` overrides the settings;
`LM_XL_SHADOW_LOG=1` logs shadowed lights / caster draws / casters skipped while compiling;
`LM_TEST_LIGHTS=<n>,<radius>,<intensity>[,<orbit speed>[,<distance>[,<height>]]]` (speed 0 =
fixed positions; all test lights are flagged to cast shadows); `AURORA_ENH_GLOWDBG=8` shows the
mask (red/green/blue/alpha = lights 0-3), `=9` the atlas depth vs receiver distance and face.

**Cost** (GPU timestamps, `AURORA_ENH_GPU_TIMING=1`: per mask, the depth pre-pass, atlas and mask
passes; a frame has two masks, the second incremental; RTX 3090 Ti at 2560x1440, shared with other
agents' game instances, so treat as upper bounds). Per frame:

| Scene | Screen-space (4) | High: 4 lights, 512, 12 taps, half-res mask | Ultra: 8 lights, 1024, 20 taps, full-res mask |
|---|---|---|---|
| Brazier room (3 shadowed lights, ~170 caster draws) | 0.4 ms | 1.0-1.2 ms | 3.2-3.8 ms |
| 1F hallway, 8 test lights all reaching every pixel | - | 1.1 ms (4 lights) | 5.3-5.9 ms |

The atlas scales with lights x face size squared x room geometry; the mask pass with mask pixels
x lights reaching them x taps (the worst case is many lights overlapping the whole screen). The
earlier frame-rate A/B (`LM_ENH_PERF_AB=6,xls`) could not resolve these against the other
processes' load, hence the timestamps.

**Before / after** (`build/lightshadows/`): window grabs at 2560x1440, Ultra, screen-space vs
shadow maps (separate runs, so poses differ): `showcase_lydia_*` (Lydia's magenta glow: the
vanity mirror and the painting cast sharp colored-light shadows on the wall),
`showcase_brazier_*`, `showcase_dining_*` (the brazier's column and the cabinet shadow the wall),
`showcase_study_*` (Neville's study, fireplace light), `showcase_luggs_*` (Mr. Luggs);
`ab_<room>_shadows_none_diff.png`: native frame dumps of the same scene with and without the
colored-light shadows (`LM_XL_SHADOWS_AB=1`: shadows only on even frames) and their difference.

### NPCs, portrait ghosts and the GBH call (world lights, fourth pass)

- **E. Gadd's lab** (map 1): no colored light sources (no flame/fire particles; the lab's own GX
  lighting is warm already); E. Gadd and Luigi use the character material
  (`build/worldlights/npc_egadd_lab.png`).
- **Portrait ghosts**: Neville (`father`, Study, 2F room 34) and Lydia (`mother`, Master
  Bedroom, room 33, appears with event flag 62) are ghost lights like any ghost; Lydia's glow
  floods her vanity corner magenta and, with shadow maps, the mirror and painting cast its
  shadows (`npc_lydia_bedroom.png`, `showcase_lydia_*`); the Study's fireplace is a fire light
  that lights Luigi and Neville from behind (`npc_neville_study.png`). Positions come from
  `jmp/characterinfo` of map 2 (`tools/jmp_dump.py`).
- **Mario's painting** (`marioe`, Secret Altar room 70, appear flag 66): not reached. Setting
  flag 66 and teleporting to the altar shows the room without the painting; it appears in the
  ending sequence, which these test aids do not reach.
- **GBH glow during E. Gadd's call**: `LM_SKIP_TEST=2500:flag:15` starts the "first Boo" call;
  Luigi lifts the Game Boy Horror to chest height (106 units above his feet, over the 105 gate)
  and its green screen glow lights his face and hands before the call screen opens
  (`build/worldlights/gbh_call_glow_off_on.png`, `LM_WORLD_LIGHT_AB=1` off / on).

### Enhanced lighting fixes (branch `enh-fixes`, Aurora 0186)

User report after a full playthrough with enhanced lighting: "the general look of everything is
very nice but the implementations are buggy". Four problems, each fixed at its cause; the plain
renderer (master switch off) is untouched (verification at the end). Images: `build/enhfix/`
(not in the repo).

**1. Ambient occlusion drew over everything in front (Luigi took the cabinet's AO).** Cause: the
opaque depth pre-pass that the AO and colored-light shadow masks are computed from kept at most
`MaxGlowDraws` = 4096 draws, but LM's main view has about 5000-7400 opaque world draws per frame
(`AURORA_ENH_LOG`: "4096 depth draws" before the copy that ends a pass of 5019 world draws). The
draws past the cap are the late ones: Luigi, the other actors, late room parts. Their pixels then
read the mask of whatever stood behind them. Fixes (0186):
- The pre-pass list has its own cap, `MaxDepthDraws` = 32768.
- The pre-pass depth goes into the mask buffer as a fourth plane (`MaskDepthPlane`, R32F bits;
  `MaskBufferSize` 48 -> 64 MiB) and a receiver takes a mask texel only if the depth stored with
  it is its own surface (depth match within the fragment's depth slope over the tap distance plus
  0.4%; `enh_fz` / `enh_dz` are set at the top of `fs_main`). The 4x4 tent is one 16-tap loop
  with normalised weights; no matching texel = unshadowed / unoccluded. This covers AO and the
  colored-light shadows (screen-space and shadow-map masks), so the half-resolution mask no longer
  bleeds across silhouettes either. Side effect: translucent receivers (not in the pre-pass) no
  longer pick up the colored-light shadow of the surface behind them.
- AO samples use a 4x4 interleaved rotation followed by a depth-aware 4x4 blur (`fs_aoblur4`)
  instead of a per-pixel noise rotation, which left a hatched pattern on flat walls.
- Debug: `AURORA_ENH_AO_SHOW=1` outputs the occlusion each surface receives (white = none),
  `AURORA_ENH_GLOWDBG=11` the raw AO mask; port `LM_AO_AB=1` (AO only on odd dump frames).

Other screen-space parts checked for the same class of bug: the colored-light shadow masks (same
mask, fixed above); bloom's glow buffer is depth-tested against the depth at the end of its pass;
the light glows test the final depth (now five taps over the glow core instead of one, so a glow
fades as an edge slides over its light instead of popping); the volumetric beam and the bloom
bright pass run at scene end on the final depth; the game's own shadow map is unaffected.

**2. Candle lights stayed lit after the candles went out.** How the game does candles (US):
`objinfo` "sfire" objects (arg0 = behaviour: 2 unlit until fire, 3 can be blown out by the
vacuum, 6 relit when Luigi leaves the room) are candle objects from the pool of `fn_80164608`
(0x50 bytes: +0x10 state, +0x30 emitter, +0x40 colour; the emitter's +0xC4 points back). The
update `fn_80164140` runs a state machine: 1 lit, 2 flickering near Luigi, 3 blown by the vacuum
(`fn_80163D58`), 4 out (relit by the fire element), 5 out until the room's light comes on
(`fn_80018D78`); going out it writes emitter flags 0x0C (emitting bit cleared), relighting 0x0E,
removal 0x1C. World lights now follow that every frame (`flameLevel` in world_lights.cpp):
- a flame counts while its emitter is drawn in a visible room; it is lit while emitting and, for
  a candle object, while its state is 1-3;
- the light fades with the flame's live particles (emitter +0x74 + +0xB0, the count the game keeps
  its own candle GX light for, `fn_801799AC`): out = down with the dying particles (~10 frames),
  relit = up as the flame grows; the first sight of an emitter takes its state without a fade;
- merged groups (candelabras) keep their unlit members, so the group's id and shadow slot do not
  change when one candle goes out; the light dims by the flames still burning and its centre leans
  toward them;
- fireplaces, torches and fire effects (no candle object) follow their emitter and particles the
  same way; lamps and chandeliers of lit rooms fade with the room's light-on bit instead of popping.

Test aid `LM_TEST_BLOWOUT=<frame>[,<relight frame>]` (main-view frames of the map): every visible
lit candle goes out / is relit with exactly the game's writes. `LM_WORLD_LIGHT_LOG` lines show each
candle's state and particle count and each light's `lit` level; `=3` also logs who creates each
emitter and where candle emitters are referenced from (research). Foyer, all 14 candles out at
main-view frame 250: the group lights go lit 3.00 -> 0.87 -> 0.02 -> gone within 20 frames and come
back after the relight at 330.

**3. Shadows blinking while idle (foyer sconce above the stairs).** The user's 120 fps clip: the
wall under the sconce alternates between the sconce's shadow and fully lit, in blocks of 1-2 game
frames (4-8 presented frames). Cause: more shadow-flagged lights than shadow slots. The port gave
the "wanted" state to the first N flagged lights by a score that includes the flame flicker and
the ghost pulse, but every light still fading kept its cast-shadows flag; with a fifth candidate
(a ghost light, or a test light, next to the foyer's four candelabras at the default High = 4
slots) Aurora saw five flagged lights and gave its slots to the first four in list order, which
the flicker reordered every frame: the sconce lost its whole shadow in alternate frames. Fixes:
- `selectLights` (enhanced_lighting.cpp): rank by a score smoothed per light id (~5 frames),
  lights already in the set rank 25% higher (no flipping at the 64-light cut), a shadow slot
  changes hands only for a 35% stronger light, and a new light gets a slot only once the light
  that lost it has faded out, so the flagged lights never exceed the slots (also in interpolated
  replays, which blend one frame's fading-out shadow with the next frame's).
- Aurora `assign_shadow_slots`: if more lights are flagged than there are slots anyway, the slots
  go to the strongest shadow weights instead of list order.
- Test aids: `LM_LIGHT_SELECT_LOG=1` (slot changes) / `=2` (every frame: the shadow-casting lights
  with their weights), `LM_XL_SHADOW_LOG=2` (casters per frame).

Reproduced at the user's spot (`LM_TELEPORT=-430,0,-10,30`, foyer start, idle, High preset, ghost
lights, `LM_TEST_LIGHTS=2,450,1.2,0` = two fixed extra shadow casters as the fifth/sixth
candidate): old build 11 jumps of more than 8 levels in 150 frames in the wall area under the
sconce (shadow / no shadow alternating, `sconce_seq_old_new.png`), new build 3 (the scripted
camera settling); the blink heat maps (`starve_heat_old_new.png`) show the sconce and banister
region blinking only in the old build. Without the extra casters the selection log shows the four
candelabras keeping full weight for the whole idle stretch.

**4. Hardware-accuracy features with enhanced lighting** (A/B in the nursery with enhanced
lighting on, `LM_ACCURACY` masks, the same frame of two runs; matrix also in docs/ACCURACY.md
section 12):

| Feature | Interaction with enhanced lighting | Decision |
|---|---|---|
| EFB color depth (6-bit + dither, RGB565/RGB5A3 copy precision) | the lit scene goes through the 6-bit EFB and the game's RGB565 full-screen copy that it redraws every frame: bloom haze, colored-light pools, AO and per-pixel gradients band into blotches (a floor crop: 63 distinct green levels instead of 247, `acc_band.png`) | **off while enhanced lighting is on** (`gx::enhancedOverridesEfbDepth`); F1 toggle greyed out and unticked with a tooltip note; the setting itself is kept |
| 6-bit blending (0176) | only exists inside EFB color depth | off with it (greyed out) |
| PE dither (0180 4x4) | part of EFB color depth | off with it |
| Early-Z cut-out split (0170/0179) | the depth-only half writes the same depth the colour half tests; the pre-pass copies each draw with its own depth state | kept |
| Pixel snap 1/12 (0180) | the pre-pass and the shadow-map casters reuse the draw's vertex shader; positions move by less than 1/12 px, inside the depth-match tolerance | kept |
| TEV DIVIDE_2 truncation (0180), integer TEV | per-stage TEV math after the enhanced lighting terms; no visible interaction | kept |
| Vertex snapshots | the pre-pass and casters re-render the recorded draw data, so they see the same snapshot as the draw | kept |
| EFB copies to RAM | the game's Z16 shadow copy is read back exactly; the enhanced offscreen shadow map still needs "Shadow map at render resolution" | kept |
| Display / texture copy filters | soften the redrawn scene vertically as on the console, enhanced output included; no artefact | kept |

Measured (nursery frame 2651, enhanced lighting with AO and ghost lights, each feature switched off
alone against a run with all accuracy features except EFB color depth; separate runs, so the ball,
the dust and Luigi's idle animation differ a little): pixels differing by more than 8 levels:
early-Z split 2.0%, pixel snap 2.4%, DIVIDE_2 truncation 1.0%, vertex snapshots 1.7%, EFB copies
to RAM 0.7%, integer TEV 0.7% (all the run-to-run level, no structure in the difference images);
EFB color depth on: 52% (the banding).

Coordination with "Smooth color banding" (`deband`, Aurora 0190): it smooths the 6-bit EFB; with the
6-bit EFB off under enhanced lighting it does not apply there (greyed out), and the remaining
quantization is the 8-bit target itself.

### Enhanced lighting fixes, second round (branch `enh-fixes-2`, Aurora 0187)

After a full playthrough with enhanced lighting. Images: `build/enhfix2/` (not in the repo).

**1. No light balls.** The visible orbs (balcony railing, above Grabbing Ghosts, in fixtures) were
the bloom's "light glows": a screen-space halo drawn at every extra light's position, hidden only
when the depth buffer showed a surface in front of it. A light whose source is hidden or never
drawn (a ghost behind the railing or inside a plant, a light lifted above a ghost's head, a lamp's
light point inside its shade) still showed it. 0187 removes the halo pass entirely (the
"Colored light glow" setting and `bloom_light_glow` key are gone); glow now only comes from pixels
that are actually drawn: the glow buffer (the game's additive draws re-rendered, depth-tested) and
the bright-surface term. The realistic flashlight's screen-space lens glare (0196) was the same kind
of sprite (with the lens hidden behind Luigi it was a bright ball around his head,
`lens_glare_before_after.png`) and is removed too; its flare still brightens the beam.

**Ghost lights only from visibly drawn ghosts.** The light used actor +0xE3 (alpha) and the draw
pass's flag tests, but not the pass's own per-type test `(actor +0x40 & 0xF) & pass` nor the alpha
the pass really draws with (some types flicker with their own value). Now the ghost layer's
`GXSetDstAlpha(1, alpha)` calls inside `fn_80060E70` (return addresses 0x800611C4 / 0x800611D4,
r21 = the actor; `hle_gx.cpp` -> `ghostlights::onGhostDrawn`) record which ghosts were drawn with
which alpha; a ghost gives light only if the previous ghost pass drew it with a non-zero alpha
(lights are built before the pass of their frame). Hidden ghosts (in furniture, not yet appeared,
fading between appearances) give none; the "Invisible ghost glow" option is removed. Verified in
the anteroom (gold ghosts, `LM_GHOST_LIGHT_LOG`): light while visible / in the beam, `hidden` and
0 between appearances.

**2. Room-lit lamps; candles independent.** World-light categories:

| Category (resource / source) | Kind | Light while |
|---|---|---|
| `candle_*` (`candle_moe`, `candle_vio`; sfire candle objects) | flame | its own state (`fn_80164140`: lit 1-3, out 4/5, emitter emitting) |
| `danro_fire`, `torch`, `tc_s*` (hearths, braziers, torches) | flame | the emitter emits (independent of the room light) |
| `chandelier`, `shanyb`, `m_light1_*` particle glows | electric | the room is lit |
| `jmp/objinfo` "chandelier", `jmp/furnitureinfo` `syan*`/`shan*`/`a_lamp*`/`lamp*`/`a_ligh*`/`a_lump*` | electric | the room is lit |
| fire effects, element sprays, sparkles, GBH screen, lightning | effects | their own state |

"Room lit" is now the game's own test `fn_80018D78(room object)`: room object +6 bit 0x80 (a
cleared room) and 0 in the mansion while flag 61 (the breaker blackout) is set (`fn_80037894`). The
port used to read only the bit, so lamps stayed on during the blackout; particle lamp glows had no
room test at all. Logged (`LM_ROOM_LIGHTS_ON=1`, foyer): 10 chandelier/lamp lights while lit, none
after `LM_SKIP_TEST=2700:flag:61`; candles unchanged. Room numbering checked: the current-room id,
the room table index and the room objects agree (foyer 2, dining 8, hallway 10, Twins' room 27).

Candles in real play (not `LM_TEST_BLOWOUT`): vacuuming next to the foyer's side-table candelabra
(`LM_TELEPORT=-420,0,-80,800`, R held) puts the candles in state 3 while the nozzle pulls, then
blows them out one by one (state 4, flags 0x0C): the group light goes 3.00 -> 2.00 -> 1.00 with its
centre leaning to the burning one. Dining room: two of each violet candelabra's three candles
start out (state 4) and the lights burn at 1.00 per candelabra. Anything else that puts a candle
out (a ghost) goes through the same emitter flags / state, so it is covered by the same path.

**5. Lights stay in their room.** `ExtraLight` gains a box (0187, flag `ExtraLightRoomBox`,
`GX_AURORA_EXTRA_LIGHT_WORDS` 10 -> 22, storage 24 words per light): the room's bounds from the
room table `lbl_804D8170`, in view space relative to the light (centre offset + three half-axis
vectors, so it follows the light when interpolation blends the position; the box itself is not
blended). The shader fades the light out over 24 units past the box. Every provider sets it
(`enh::limitToRoom`): flames and lamps by their position's room, ghosts by theirs (room byte as
fallback), spray / GBH / lightning by Luigi's room. Test aid `LM_ROOM_BOX=0` (off) / `2` (boxes
shrunk to a quarter around the room centre: the cut shows where the box is, `room_box_check.png`).

**3. Shadow flicker.** Idle captures (every frame, 120 frames) in the foyer, dining room, ballroom
and anteroom at 30 fps, and the interpolated in-betweens at 120 fps (`LM_INTERP_TEST=4`), with a
blink detector (luminance jumps that reverse within 1-3 frames; the GBH screen's static excluded).
The remaining shadow blinks came from shadow-slot hand-overs: in the dining room a gold ghost's
light (its score drops to a fifth while it fades between appearances) and a candelabra traded a
slot every ~1 s (42 slot changes in ~2000 frames), each a fade-out/fade-in of two shadows. Fixes
(`selectLights`): slot decisions use a peak-held score (decays to half in ~1.5 s), a holder keeps
its slot at least 45 game frames, and a holder that disappears (a ghost hiding, a culled light)
keeps its slot reserved for 20 frames. Same peak-hold for the ghost provider's own shadow ranking.
Ballroom 133 -> 28 slot changes; at 16 slots only reappearing ghosts gain slots (no hand-overs).
The 120 fps in-between sequences showed no shadow blinks (dancers: motion only).

**4. Ambient occlusion.** The user's builds had the 0186 depth-prepass fix. Remaining issue: the
range term `radius / |dz|` still weighted an occluder far in front of the receiver (10-50%), so the
wall around a character's silhouette darkened (a halo following Luigi; `AURORA_ENH_AO_SHOW=1`,
`ao_show_dining_before_after.png`). Occluders now count only within the radius in 3D (hard falloff
from 1x to 1.5x the radius): contact darkening stays (floor at Luigi's feet, corners, under
furniture), halos are gone. Luigi himself receives none from what is behind him (checked in the
show mode while walking in the dining and Twins' rooms). The half-resolution mask (High) still
shows small steps on thin cut-out frames (the dining cabinet's glazing bars); Ultra's mask is full
resolution.

**6. Banding.** With enhanced lighting the 6-bit EFB is off and so is the RGB565 precision of the
game's scene copy (`GX_AURORA_ACCURACY_EFB_DEPTH` gates the copy quantization in `recording.cpp`).
Checked on enhanced frame dumps (dining, Twins' room, anteroom): 255-256 distinct levels per
channel and the pixels are not on the RGB565 lattice (R 0.11-0.16 of pixels, the random rate is
0.125; G 0.26-0.30 vs 0.25), so the copy does not band and the smoothing stays off there.

**More shadow-casting lights.** "Shadow-casting lights" offers 12 and 16 (`extra_shadow_lights` 4,
5). `MaxShadowLights` 16, four RGBA8 mask planes (slots 0-15; AO plane 4, depth plane 5,
`MaskBufferSize` 128 MiB), the mask pass writes four targets, the uniform's light-shadow block is
`xl[36]` (`xl[s]`, `xl[16 + s]`, `xl[32]`, `xl[33]`), and the atlas is a near-square grid of cube
maps within the device's texture limit (16 lights at 1024: 9216 x 12288). With more than 8 slots,
single candles and lit lamps are shadow candidates too. GPU time per mask (`AURORA_ENH_GPU_TIMING`,
16 fixed test lights, 1024 faces, High quality, 1280x960, GPU shared with other agents):

| Lights | Pre-pass | Atlas | Mask | Total | Atlas memory |
|---|---|---|---|---|---|
| 4 | ~0.5 | ~1.2 | 0.17 | ~1.8 | 100 MB |
| 8 | ~0.2 | ~1.0 | 0.37 | ~1.5 | 200 MB |
| 12 | 0.16 | 1.13 | 0.56 | 1.85 | 300 MB |
| 16 | 0.16 | 1.48 | 0.74 | 2.4 | 450 MB |

Defaults unchanged (High 4, Ultra 8).

**Enhanced off.** Two runs of the nursery script with the master switch off (main vs this branch)
reach the same frame with one pixel different (dust); runs differ from each other by timing on a
loaded machine, the same way for both builds (`enhanced_off_same.png`).

### Realistic flashlight (branch `flashlight-beam`, Aurora 0196)

User report: "the light cone on Luigi's flashlight barely looks different in our enhanced graphics
mode". The previous enhanced beam was a haze ray-marched through the GX spot attenuation on top of
the game's own cone mesh, with no occlusion, and the pool on the walls was the game's flat spot
term. F1 > Visuals > Enhanced lighting > Atmosphere > **Realistic flashlight**
(`realistic_flashlight` Off/Medium/High/Ultra, default High; `beam_density` 0..3, `beam_dust`
0..2). It replaces the volumetric beam while on (that option is greyed out); with it Off, enhanced
lighting renders as before (nursery frames match main's build apart from the random dust
particles), and the plain renderer is untouched (nothing is sent with the master switch off).

**The game's flashlight** (US, nursery; `AURORA_BEAM_LOG=1` logs it every scene end): GX light 7,
colour (255, 220, 100), angular polynomial (0, -4.53, 5.53) = an edge at cos 0.819 (35 degrees),
distance attenuation (1, 1e-5, 0), i.e. **no distance falloff at all**: the pool is equally bright
at 200 and 1500 units, which is why it read as a flat tint. A white, wider light (edge cos 0.766)
appears for ~90 frames at the start of some scenes. The ghost stun has no GX signature (the light
does not change); see "Flare" below.

**Parts** (`lib/gfx/enhanced.cpp` `fs_beam`, `lib/gfx/beam_profile.hpp`, `lib/gx/shader.cpp`):
- **Light scattering against the flashlight's shadow map.** The game renders one Z16 shadow map per
  frame, from the flashlight in dark rooms ("Shadows" above). Aurora takes it from the first
  main-view receiver draw (TEV GR16 compare stage: shadow map texture, swap tables) and turns its
  two projective POS texgens (map s,t,q and depth ramp s,q: texture matrix, post matrix and the
  inverse of the draw's position matrix) into view space, so any point in the air is tested
  exactly as the receivers test surfaces (receiver key floor(65536 s/q) > packed map key, 2x2
  bilinear PCF). The map is used only when its centre of projection lies within 120 units of the
  flashlight (lit rooms and lightning use another light; the haze is then unshadowed). Per pixel
  of a half-resolution target the view ray is clipped to the cone (analytic ray-cone interval,
  widened to the spill) and to 1500 units around the lens, then marched (20/32/48 steps, fixed
  per-pixel jitter) with the reflector profile below, inverse-square falloff (regularised by a
  40-unit lens), air extinction `exp(-r (0.0007 + 0.15 sigma))` (the beam fades within a few
  metres; denser air fades faster), a Henyey-Greenstein phase (g 0.35 mixed with isotropic:
  looking into the lens is brighter), a world-anchored drifting haze density (two octaves of
  periodic value noise, 9 samples per ray, interpolated) and the shadow test. Luigi's hand, the
  crib bars, the rocking horse, bears, furniture and ghosts cut visible shafts. A depth-aware
  separable 9-tap blur removes the jitter; the composite upsamples depth-aware (4 taps weighted by
  view depth) so the haze stops cleanly at silhouettes in front of it, and adds
  `colour x (1 - exp(-v))` (a little whiter where dense) before the HUD.
- **Dust motes**: one possible mote per 32-unit world cell (PCG hash; `beam_dust` sets the
  density), found with a 3D DDA along the lit part of the ray within 900 units of the lens; each
  drifts slowly inside its cell, twinkles, and is lit only by the beam (profile x falloff x shadow).
- **Reflector footprint on surfaces** (enhanced shader variants, `enh_flags` bit 5, bright spot
  cones of colour channels, per pixel): the GX angular term is replaced by
  `enh_beam_profile(angle / edge angle)`: a hot centre (1.65 on the axis), a slightly darker ring
  around it, an even corona (~0.75) with faint concentric reflector rings, a soft edge and a dim
  spill just outside the game's cone; times an inverse-square falloff referenced to 650 units
  (`(650^2 + 40^2) / (d^2 + 40^2)`, clamped to 0.25..2.2) and a gain of 1.2. The lit channel's
  clamp turns the centre white-hot (the yellow light reaches (1, 1, 0.6)) while the corona keeps
  the yellow; walls near Luigi are brightly lit, far walls dimmer. Alpha channels (the shadow
  light's ALPHA1 mask) stay exact. The same profile shapes the scattering, so shafts and pool
  match. Soft contact-hardening shadows on surfaces are the existing PCSS soft shadows.
- **The game's cone mesh** (`handlight_ef`, additive, alpha from a colour-texgen lookup) is
  **hidden** while the realistic beam is on (no double cone): its draw keeps only the EFB
  **alpha** write, because the game's later full-screen passes read that alpha. Dropping the draw
  entirely tinted the beam area with the room's blue darkening (found in the first A/B;
  `AURORA_BEAM_NOHIDE=1` keeps the mesh for comparison). The lens sprite, the vacuum and the Game
  Boy Horror are separate draws and unchanged.
- **Flare**: the beam flares (scattering x(1 + 2.5 f), whiter; surface gain x(1 + 0.6 f)) and
  decays in ~0.4 s. (The screen-space lens glare with core, halo and streak was removed in
  `enh-fixes-2`: enhanced lighting never draws a halo at a light's position; with the lens hidden
  behind Luigi it showed as a bright ball around his head. See "Enhanced lighting fixes, second
  round".) Triggers: a ghost's AI receiving the "in the flashlight beam" message
  (EnThought message 2, `fn_800C0F08` hook) for the first time after 45 frames, i.e. when the light
  catches and stuns it (`aurora_enhanced_beam_flare`); the flashlight switched on after being off;
  any jump of the light's brightness above its recent level.
- **Interpolation**: everything comes from data frame interpolation already blends per presented
  frame: the GX light (flashlight), the camera projection, the receivers' shadow texture matrices
  (rebuilt from the blended light, `LM_SHADOW_SNAP=3`) and the world anchor, which the port sends
  as three pseudo extra lights (`AURORA_EXTRA_LIGHT_ANCHOR`, ids `0xBEA0A000+k`: view positions of
  the camera target snapped to 1024 units and of points 256 units along world X/Y; interp blends
  them by id like lights, Aurora strips them from the light set). Dust and haze drift on
  wall-clock time, so they move smoothly at any frame rate. Aspect ratios and render scales: the
  pass works in render-target pixels with the game's projection; the target is half resolution
  capped at 540/720/1080 lines (Medium/High/Ultra).

**Verification** (images in `build/flashlight/`, not in the repo; 640x480 frame dumps of the
nursery script with `LM_BEAM_AB=1`: even dump frames = previous enhanced beam, odd = realistic):
`nursery_old_new.png` (door across the room, crib bars, near a wall, rocking horse, lightning),
`nursery_beam_only_shafts.png` (`AURORA_BEAM_SHOW=1`: the scattering alone with the shafts of the
crib bars and the horse), `nursery_turn_sequence.png` / `nursery_sweep_sequence.png` (consecutive
frames turning and sweeping), `ballroom_old_new.png` (chairs, dancing ghosts in the beam),
`lit_room_old_new.png` (`LM_ROOMS_LIT=1`: shadow map from the room light, unshadowed haze),
`ghost_stun_flare.png` (gold ghost room: beam on the ghost, stun with heart, flare),
`flare_sequence.png` (`LM_BEAM_FLARE_AT`), `presets_medium_high_ultra.png` / `presets_crop.png`
(render scale 3), `ultrawide_21x9_old_new.png` (21:9, render scale 3).
Interpolation at 120 fps (`LM_INTERP_TEST=4`, nursery frames 2590-2700,
`AURORA_BEAM_FREEZE_TIME=1`, tools/interp/metrics.py): **0 stale pixels** (every late replay equals
its 30 Hz frame); the "back-and-forth" count is higher than with the option off (42k vs 24k px over
110 frames) because the narrow hot centre passes over a wall within one game interval as the beam
sweeps (frame 2655: the pool slides across the wall in the in-between frames); no flicker between
replays.

**Cost** (GPU timestamps of the beam passes: `AURORA_ENH_GPU_TIMING=1` logs "flashlight GPU";
nursery, 2560x1920 render target, RTX 3090 Ti shared with several other game instances at 100% GPU
load, so upper bounds): previous volumetric beam 0.33 ms; Medium 0.5-0.9 ms, High 0.6-1.4 ms,
Ultra 2-3 ms, depending on how much of the screen the beam covers; nothing while the flashlight is
off. Dust 0 vs 1 at Ultra: within the noise. The footprint adds a few ALU ops per spot light in
shaders that already evaluate it per pixel.

**Test aids**: `LM_REALISTIC_FLASHLIGHT=<0..3>` (override), `LM_BEAM_AB=1` (realistic only on odd
dump frames), `LM_BEAM_FLARE_AT=<dump frame>[,<period>]`, `LM_BEAM_LOG=1` (each flare with its dump
frame), `LM_ENH_PERF_AB=<s>,beam`; Aurora `AURORA_BEAM_SHOW=1` (the scattering alone) / `=2` (the
dust alone, amplified), `AURORA_BEAM_LOG=1` (flashlight light, cone edge, flare, shadow-map use,
every scene end), `AURORA_BEAM_SIGMA=<x>` (scattering coefficient at density 1, default 1.4e-3),
`AURORA_BEAM_FREEZE_TIME=1` (deterministic dust and haze for frame comparisons),
`AURORA_BEAM_NOHIDE=1`, `AURORA_BEAM_NOPASS=1`.

**Open**: the stun trigger is the first in-beam message (the light-bind that actually stuns is
strategy-specific: gold ghost +0x590); element ghosts, bosses and cutscene lights with the beam on
were not checked; dust and flare run on wall-clock time, so frame dumps of separate runs differ in
them unless frozen.

#### The beam detached from the flashlight (branch `flashlight-fix`)

User report on the merged build: "the new realistic flashlight cone frequently detaches from the
flashlight". Cause: the beam took its origin and direction from "the bright spot light nearest the
camera" among the frame's world draws, and that is often not the flashlight's lens.
Measured with `AURORA_BEAM_LOG=1 LM_BEAM_LOG=1` (every scene end: the lens of the game's cone mesh,
the spot lights seen, and how far the first version's choice was from the lens):

| Situation | What the first version used |
|---|---|
| Nursery, about one scene end in five | a second spot light with the same direction 216 units above the lens (a light the game uses for some models) |
| Gold-ghost room, whole stretches (e.g. dump frames 2624-2694, 3050-3122 of the ghost script) | a scene light 1100+ units away pointing ~138 degrees elsewhere: the haze appeared as a blob at the far wall |
| Luigi entering a room, turning on the spot, startled (nursery 2244-2316, 2490-2562) | the game's own flashlight light, which there trails the hand by up to ~220 units (smoothly) |
| Interpolated replays (120 fps) | the light blended as a light while the hand is blended as a model matrix; in the scripted nursery runs they agreed to within 15 units, in an earlier run up to ~13 units / 2 degrees |

Not causes: in ordinary play the game builds the flashlight GX light from the cone mesh's matrix in
the same frame (position = the matrix translation, direction = model -X: equal to float precision,
nursery), so the light is neither lagged by a frame nor at Luigi's head there; the shadow map's
light is the game's light (the beam's shadow test keeps using the receivers' matrices, so its
shadows still match the surfaces).

**Fix** (0196, `resolve_flash` in `enhanced.cpp`): the beam is welded to the game's own cone mesh
(`handlight_ef`, the draw that is hidden while the beam is on): its position matrix gives the lens
(translation) and the axis (-X). That matrix is one of Luigi's model matrices, so frame
interpolation blends it with his hand and arm. The spot parameters (cone, colour, distance terms)
come from the flashlight light: the bright spot light within 400 units of the lens pointing within
30 degrees of the axis (the trailing case included). When no such light exists (the lift between
floors, scripted scenes: the scene is lit by other lights and the flashlight is only the mesh) the
realistic beam is skipped and the game's cone mesh is drawn as in the original (decided from the
previous frame, since the mesh is drawn before all lights are known). Without a cone mesh in the
frame (cutscene lights such as the opening's) the previous choice was kept; since 0220 there is no
beam then (see "The white wedge in the foyer" below). The cone mesh is lit by a
dummy light 7 at (0, 0, 10000), so its own lighting cannot identify the flashlight. The shadow-map
check compares the map's eye with the flashlight light (not the mesh), so a trailing light keeps
its shafts. The beam's inverse-square term is capped at 4x the reference level, so a camera looking
down the beam from close by (it happens in the lift) is not washed out to white.

Images (`build/flashlight-fix/`, consecutive dump frames of one run with `AURORA_BEAM_NOCONE=2`:
odd frames = first version, even = fixed): `ghostroom_ab_2640_crop.png` and
`ghostroom_ab_2640.png` (old: the haze sits at the far wall, off the hand; new: the beam leaves the
lens), `nursery_entry_ab_2500_crop.png` / `nursery_entry_ab_2500.png` (Luigi walking in: old: a
glow in front of him from the trailing/other light; new: from the lens),
`ghostroom_lift_ab.png`, `ghostroom_ab_2895.png` (idle, both agree),
`nursery_120fps_vacuum_gbh_turns.png` (120 fps `LM_INTERP_TEST=4` replays: vacuum, Game Boy Horror
open/close, quick turns; the lens-to-light distance in every scene end of gameplay stayed below
15 units, the beam stays on the lens). Interpolation: 0 stale pixels as before.

Test aids: `AURORA_BEAM_NOCONE=1` (first version's choice) / `=2` (on every other scene end,
logged), `AURORA_BEAM_LOG=1` (cone lens, spot lights, match distance and angle), `LM_BEAM_LOG=1`
(port: dump frame of each main view, to pair with the Aurora lines).

#### The white wedge in the foyer (Aurora 0220)

User video: in the foyer Luigi stands by the pillar right of the web-covered door and the beam
shows as a huge, flat, hard-edged white wedge over the left of the picture, "detached from the
flashlight", identical at Original 30. Reproduced with the integrator's wander script (map 2 =
foyer, `LM_TEST_STATE=poltergust+water`, the user's Ultra settings, `AURORA_BEAM_FREEZE_TIME=1`):
dump frames 2133 and 2203-2207.

**Cause: the beam was drawn from a light that is not the flashlight.** Those are the frames where
Luigi walks into the electrified barrier: the game turns the flashlight off (no `handlight_ef`
cone mesh is drawn; vanilla shows no beam at all) and lights Luigi with a white, wide spot light
(colour 1/1/1, angular edge cos 0.766, distance term k1 = 1/1900) at his position, pointing back
at the camera. Without a cone mesh `resolve_flash` kept the frame's nearest bright spot light, so
the realistic beam was ray-marched from that light: white, unshadowed (the game's shadow map is not
rendered from it), its apex hidden behind the pillar so it seemed to start in mid-air, and boosted
about 2x by the switch-on flare (the flashlight had been off for over 0.4 s). This is also the
"white, wider light at the start of some scenes" noted above. The shader itself was not at fault:
the march is clamped to the scene depth (the pillar is not painted over by air behind it), the
shadow test and the Henyey-Greenstein phase (at most 3.2 at g = 0.35) are bounded.

**Fix:** a frame without the cone mesh has no flashlight: no realistic (or volumetric) beam, and
it counts as "off" for the switch-on flare (`resolve_flash` now runs before anything reads
`g_flash`). Gameplay never relied on the fallback (an opening-to-foyer run had no beam frame
without the mesh; the wander run had only these 6).

**Looking into the beam.** With the real flashlight aimed at the camera (Luigi walking toward it in
the nursery, frames 2672-2792 of the nursery script; foyer frame 2140, camera 20 degrees off the
axis, i.e. inside the cone, 1440 units from the lens) the view ray runs lengthwise through the
bright part near the lens and the haze it gathers grows like 1/sin(angle to the axis); the
picture was a full-screen yellow wash. `fs_beam` now scales the scattering of a ray within the
cone's half angle of the way back to the lens by `min(sqrt(sin^2(angle) / sin^2(half angle) + 0.36), 1)`
(0.6 straight into the lens): the glow stays centred on the lens and the room stays readable.
Rays at larger angles (every side or back view of the beam) are unchanged, bit for bit.

**The yellow patch on the upper wall** after the shock (dump frames 2208-2213) is the flashlight's
own pool: Luigi aims it up and slightly away (axis about (-0.07, 0.98, -0.17) in view space), and
vanilla lights the same patch (same outline: the balcony above, the pillar's silhouette in front).
The enhanced footprint is brighter near Luigi (inverse square) as designed; not an artefact.

**Verification** (640x480 frame dumps, Original 30, images in the agent scratchpad, not in the
repo): foyer before/after (2133, 2203, 2205, 2207, 2140): the wedges are gone (frames as vanilla
plus the room's enhanced lighting), 2140 a softer lens glow; frames where the flashlight is on and
not facing the camera are identical, the rest differ by at most 23 levels where the flare history
changed (the shock no longer counts as switching the flashlight on). Nursery script (Ultra, every
4th frame 2580-2960): identical except the 19 frames where Luigi faces the camera. Vanilla
(enhanced lighting off) 2190-2240: byte-identical to main.

Test aid: `AURORA_BEAM_SHOW=3` shows the scattering amount v as a heat map (red 0..2, green 2..6,
blue 8..16) instead of the picture.

### Ambient occlusion, horizon-based (branch `ao-gtao`, Aurora 0224)

User report: the forward AO (0032/0186/0187) shaded some characters "polygonal and blocky"; it was
switched off (93255c4). Images: `out/ao/` (not in the repo).

**Cause.** The AO was 12 hemisphere samples oriented by the normal reconstructed from the
pre-pass depth (`mask_normal`: the flatter neighbour on each axis), i.e. the triangle's face
normal. On a round low-poly model (Toad's cap and body, E. Gadd's head, Luigi's arms and legs)
every facet then got its own constant grey (`AURORA_ENH_AO_SHOW=1`: `ao_show_toad_before_after.png`),
and samples off a facet near a silhouette landed behind the model's own front surface, so the
facets near the outline were darker still. Walls had blotches from the same hemisphere sampling.

**New AO** (all in the mask pass, still forward: only opaque world draws receive it):

| Part | What |
|---|---|
| Normals | The depth pre-pass also writes the interpolated vertex normal (`depth_normal_layout`: a write-masked placeholder color + the gx shader's existing normal attachment, RGBA8). Draws without normals and uncovered texels fall back to the depth-derived normal (its one-sided normal at one-pixel ledges: the view vector there drew a dashed dark line along rug edges) |
| Horizon search (`fs_aomask`) | GTAO-style (Jimenez et al. 2016): two view-aligned slices per pixel, each searched on both sides over 6 depth samples quadratically spaced out to the projected radius (capped at 15% of the mask height), and the visible arc integrated against the projected normal's cosine. Angle bias 0.12 rad and a height bias (4-10% of the radius plus depth) so facets, decals, rug edges and depth precision make no creases. Thickness: 3D falloff to the radius, offsets toward the camera count double (a character in front of a wall does not halo it), and a horizon decays toward lower samples after it (thin rails and legs) |
| Resolution | Half the mask's (mask above 900 lines; the user's Ultra mask is 2560x1440, so 1280x720), else the mask's |
| Noise / denoise (`fs_aodenoise`, `fs_aodenoise2`) | Slice rotation and step jitter follow a 4x4 interleaved pattern (32 directions per block, the same every frame: no temporal noise, stable when the camera stops and across interpolated replays). Two 4x4 depth- (plane distance) and normal-aware box passes at AO resolution (a 7x7 tent in all; one pass left blotches where an occluder covers only some directions) |
| Upsample (`fs_aoblur4`) | To the mask: the 2x2 AO texels around each mask texel, bilinear x the same plane-distance/normal weights against the texel's own surface. The receivers' depth-matched 4x4 tent (`enh_mask`, 0186) is unchanged |
| Incremental masks (0211) | The passes recompute within 1/2/3/4 tiles of changed pre-pass depth (upsample / second denoise / first denoise / horizon), each covering what the next reads |
| Precache | The pre-pass variants are built for the normal target too while AO is on (`PrecacheDepthNormalTarget`, 0217 expansion); the AO pipelines are built in `init_gpu` with the other effect passes |

F1 > Visuals > Enhanced lighting > "Ambient occlusion" (intensity, radius sliders) is back, on by
default with the master switch (master off by default: vanilla is untouched). Output mapping as
before: darkening = 1 - visibility^(1 + 2 x intensity).

Test aids: `LM_AO=0/1` overrides the setting; `AURORA_ENH_AO_SHOW=1` (occlusion each surface
receives), `LM_AO_AB=1` (AO on odd frames only); new `LM_SHOT=<dir>,<first>,<last>[,<step>[,<count>]]`
saves the game image at render resolution (before post-processing and UI) of those guest frames
as `.ppm` (Aurora `aurora_request_screenshot`; use 30 fps for exact game frames; `count` 2 with
`LM_AO_AB` gives off/on pairs; `LM_DUMP_EXIT=1` quits after the range).

Verified (US, the user's settings: Enhanced Ultra, render scale 4 = 3413x1920, mask 2560x1440):
Washroom Toad (`LM_START_MAP=2 LM_TELEPORT=-2210,0,-4150,100`), E. Gadd in the lab
(`LM_START_MAP=1`, frame 3050), Luigi in the nursery: characters shade smoothly (no facets),
contact shadows under feet, the ball, the crib and the shelf bears, wall/floor creases, no halo
around characters. 120 fps `LM_INTERP_TEST=3` nursery walk (frames 2610-2690): within-interval
back-and-forth pixels median 2016 with AO vs 2100 without (no added flicker). Vanilla
(enhanced lighting off) nursery frames 2600-2660: byte-identical to main (21 dumps).

Cost (`AURORA_GPU_PROFILE`, nursery, two masks per frame, GPU shared with other runs so +-30%):
| Run (same route, alternating builds) | EFB pass 1 (reference) | AO passes per frame | AO / EFB pass 1 |
|---|---|---|---|
| new, quietest run | 2.55 ms | 0.82 ms (horizon 0.30, denoise x2 0.26, upsample 0.26) + pre-pass normals ~0.03 | 0.32 |
| old (0187), quietest run | 3.02 ms | 1.14 ms (mask 0.86, blur 0.28) | 0.38 |

So about the old cost: ~0.8 ms per frame at the user's 3413x1920 (two masks per frame in the
nursery, the second incremental). The mask is capped at 1440 lines, so true 4K costs the same.
Mask resolution below 900 lines (High and below: half-resolution masks) runs the horizon pass at
the mask's own resolution.

## GPU cost (branch `gpu-opt`, Aurora 0210-0213)

Goal: the same picture for less GPU time, in every configuration (vanilla, each enhanced-lighting
preset, any resolution and frame rate). Every change below is byte-identical in frame dumps
(verification at the end); none adds an option. Interpolation replays (each presented frame
re-runs the whole tape, so every pass below runs at the presented rate) were left to the
`interp-final` branch; see "Not done".

### Measuring: `AURORA_GPU_PROFILE=<seconds>` (0210)

The pass-timestamp profiler (`lib/webgpu/gpu_prof.cpp`) now also works without Tracy:
`AURORA_GPU_PROFILE=5` logs every 5 s the GPU ms per presented frame of every pass, grouped by
label (the trailing EFB pass index dropped, `xN` = passes per frame), the busiest single passes,
the summed top-level busy time and its share of wall time (`busy ... = NN% of wall time`, which
tracks the GPU utilisation the user sees). Every enhanced-lighting pass, the post chain (`PostFx
<n>`, the last pass is in `Present blit`) and the 6-bit resolves (`EFB blend6 copy / quantize`)
are labelled. Notes: timestamps measure the GPU at its current clock, so a lightly loaded GPU
(vanilla at 1080p) clocks down and fixed passes look slower; compare busy time at similar load, or
pass by pass within one run. `AURORA_BEAM_FREEZE_TIME=1` now also steps the flashlight flare by a
fixed 1/30 s per scene end, so lit-room dumps of separate runs agree (the wall-clock flare made
the beam brightness differ run to run; main's own reruns differed there too).

### What was expensive (RTX 3090 Ti, 3840x2160, 120 fps target, user settings: Ultra-level
enhanced lighting + AO + ghost/world lights + CRT, Original 30 logic, nursery)

Busy 8.6 ms per presented frame (85-97% GPU in the four rooms, 94-114 fps instead of 120): EFB
passes 2.5-3.8 (the game's own scene, per-pixel lighting, receivers), realistic flashlight
scattering 1.4-2.1 (1920x1080 target at Ultra), ambient occlusion mask 0.8-1.7 (+ blur 0.2-0.4;
two masks per frame in the nursery and lit rooms), light shadow-map atlas + mask 0.6-1.3,
present/CRT 0.6, small passes (copies, snapshots, bloom) ~1. Vanilla at 4K: 3.5 ms busy, of which
the 6-bit blend resolves were ~1.3 ms (6-7 per frame, each a full-target copy plus a full-screen
quantize pass) and the game's EFB copies 0.4.

### Changes

| Patch | What | Exact because |
|---|---|---|
| 0211 | **Incremental second mask.** A frame's second light-shadow/AO mask (a later EFB pass with new opaque world draws) adds only the new draws to the depth pre-pass (also with AO alone, which re-rendered every draw), and recomputes AO and the shadow-map mask only within one 8x8 tile of where the pre-pass depth changed (`fs_dirty`: tile flags from the previous and the new depth snapshot, gathers; `mask_needed` discards elsewhere so the frame's first mask stays) | the second mask is read only by the later pass's own draws, at their own depth-matched texels (`enh_mask`), which are exactly where their depth landed in the pre-pass; tile + 3x3 neighbourhood covers the 4x4 receiver tent and the 4x4 AO blur. `AURORA_ENH_MASK_FULL=1` = complete masks (A/B) |
| 0211 | AO: each sample's depth loaded once (the view position was re-read through `mask_view_pos`), same in the 4x4 blur; shadow-map mask: the face record's matrix loaded once per light, not per PCF/PCSS tap | same values, fewer loads |
| 0211 | Realistic flashlight: the shadow map's 2x2 footprint per step = two gathers (the key's two bytes) at the footprint's shared corner instead of four loads; the periodic hash's modulo by a power of two is a bit mask | gather at the corner selects exactly texels b..b+1; clamp-to-edge = the clamped loads |
| 0212 | **6-bit resolves nobody sees are dropped.** A pass whose color nothing reads afterwards (no color copy, snapshot or scene op) is resolved before the next op, or not at all when that op is the next pass on the same target and clears its color completely (the shadow-map caster pass, which the game renders into the EFB and copies only as Z16) | the dropped quantization would be overwritten by the clear before anything reads it. `AURORA_EFB_BLEND6_NODEFER=1` (A/B) |
| 0213 | Scene end: bloom's bright pass samples the EFB's single-sample color directly instead of a full-resolution copy of it (the copy was also made with bloom off); receivers test an extra light's reach before loading its flags | same texels |

### Results (RTX 3090 Ti, `AURORA_GPU_PROFILE`, 120 fps target, frame interpolation, averages of
the last 20 s in each room; "before" = main + the 0210 profiler only, "after" = this branch)

GPU busy ms per presented frame, GPU utilisation (busy / wall time) and presented fps, then the
pass groups (ms per presented frame):

| Configuration | Busy ms | GPU use | fps | Flashlight | AO | Light shadows | 6-bit resolves | EFB passes | Copies/snapshots | Bloom/glow/composite | CRT/post/present |
|---|---|---|---|---|---|---|---|---|---|---|---|
| user settings, nursery 4K | 8.12 -> 6.71 | 94% -> 80% | 115 -> 120 | 2.18 -> 1.75 | 2.12 -> 0.76 | 0.22 -> 0.28 | - | 2.28 -> 2.69 | 0.49 -> 0.42 | 0.24 -> 0.24 | 0.58 -> 0.59 |
| user settings, foyer 4K | 8.46 -> 7.74 | 96% -> 92% | 113 -> 119 | 2.18 -> 1.81 | 1.07 -> 0.67 | 0.80 -> 0.73 | - | 3.05 -> 3.22 | 0.54 -> 0.48 | 0.25 -> 0.25 | 0.58 -> 0.59 |
| user settings, ballroom 4K | 8.02 -> 7.42 | 90% -> 88% | 112 -> 118 | 1.72 -> 1.43 | 0.94 -> 0.61 | 1.05 -> 0.97 | - | 3.03 -> 3.17 | 0.46 -> 0.41 | 0.24 -> 0.24 | 0.58 -> 0.59 |
| user settings, lit dining room 4K | 10.67 -> 8.92 | 97% -> 96% | 91 -> 108 | 1.87 -> 1.61 | 1.96 -> 0.75 | 1.60 -> 1.31 | - | 3.86 -> 3.94 | 0.55 -> 0.49 | 0.24 -> 0.24 | 0.58 -> 0.58 |
| vanilla defaults, foyer 4K | 4.24 -> 4.19 | 51% -> 50% | 120 -> 120 | - | - | - | 1.45 -> 1.32 | 1.90 -> 1.96 | 0.56 -> 0.57 | - | 0.33 -> 0.34 |
| Low, foyer 4K | 4.36 -> 4.34 | 52% -> 52% | 120 -> 120 | 0.87 -> 0.72 | - | 0.18 -> 0.17 | - | 2.37 -> 2.52 | 0.52 -> 0.47 | 0.25 -> 0.26 | 0.18 -> 0.18 |
| Medium, foyer 4K | 4.38 -> 4.35 | 52% -> 52% | 120 -> 120 | 0.87 -> 0.72 | - | 0.19 -> 0.18 | - | 2.37 -> 2.53 | 0.52 -> 0.47 | 0.25 -> 0.26 | 0.17 -> 0.18 |
| High, foyer 4K | 4.53 -> 4.39 | 54% -> 53% | 120 -> 120 | 0.82 -> 0.66 | - | 0.26 -> 0.24 | - | 2.54 -> 2.61 | 0.50 -> 0.46 | 0.24 -> 0.25 | 0.17 -> 0.17 |
| Ultra, foyer 4K | 5.05 -> 4.79 | 60% -> 58% | 120 -> 120 | 0.81 -> 0.65 | - | 0.75 -> 0.68 | - | 2.58 -> 2.61 | 0.51 -> 0.46 | 0.24 -> 0.24 | 0.16 -> 0.16 |
| vanilla defaults, foyer 1080p | 2.16 -> 2.19 | 26% -> 26% | 119 -> 120 | - | - | - | 0.55 -> 0.47 | 1.06 -> 1.13 | 0.46 -> 0.50 | - | 0.08 -> 0.09 |
| Low, foyer 1080p | 4.21 -> 4.05 | 50% -> 48% | 120 -> 120 | 1.11 -> 0.90 | - | 0.30 -> 0.29 | - | 2.05 -> 2.14 | 0.44 -> 0.40 | 0.23 -> 0.24 | 0.08 -> 0.08 |
| Medium, foyer 1080p | 4.24 -> 4.14 | 51% -> 50% | 120 -> 120 | 1.09 -> 0.92 | - | 0.32 -> 0.32 | - | 2.05 -> 2.19 | 0.47 -> 0.38 | 0.23 -> 0.24 | 0.08 -> 0.08 |
| High, foyer 1080p | 4.31 -> 4.33 | 52% -> 52% | 120 -> 120 | 1.00 -> 0.86 | - | 0.44 -> 0.45 | - | 2.14 -> 2.35 | 0.44 -> 0.37 | 0.21 -> 0.22 | 0.07 -> 0.08 |
| Ultra, foyer 1080p | 4.26 -> 4.24 | 51% -> 51% | 120 -> 120 | 0.84 -> 0.71 | - | 0.99 -> 0.97 | - | 1.78 -> 1.90 | 0.38 -> 0.39 | 0.19 -> 0.20 | 0.07 -> 0.08 |

"User settings" = the user's settings.ini (Ultra-level quality, AO, ghost and world lights, realistic
flashlight Ultra, CRT) with enhanced lighting on, window 3840x2160, 120 fps target, VSync off.
Presets = default settings plus the preset (AO and ghost lights are off by default, so their
masks do not run). Reading the table:

- The big wins are where the frame had a second mask (nursery, lit room: -1.2 to -1.4 ms of AO,
  -0.4 ms of shadow-map mask) and the flashlight scattering (-0.15 to -0.45 ms in every preset).
  The user's lit room went from 91 to 108 fps, the other rooms reach (or nearly) the 120 target.
- The EFB passes did not get more expensive: their shaders are unchanged (an A/B build without
  0213's one-line receiver change measured the same). Timestamps measure the GPU at its current
  clock, and with idle time per frame (the 120 fps cap reached, util 80% instead of 94%) the GPU
  clocks down, so unchanged passes read up to ~15% longer; in the lit room, which stays GPU-bound
  (96-97%), the EFB passes differ by 2%. Compare the busy time and fps at similar load.
- Vanilla: the 6-bit resolves are the largest accuracy cost (6-7 per frame, ~1.3 ms at 4K); 0212
  removes one per frame. The rest of vanilla's GPU time is the game's own passes and copies.

### Verification (frame dumps, `tools`-style deterministic runs: `LM_INPUT_CLOCK=frames`,
`AURORA_BEAM_FREEZE_TIME=1`, the same frame of the reference and the branch)

| Run | Frames | Result |
|---|---|---|
| user settings (enhanced Ultra-level + AO + lights), render scale 4, nursery / lit dining room / ballroom / foyer, 30 fps | 39 / 33 / 33 / 31 | all byte-identical |
| Low preset foyer, High preset nursery, render scale 4 | 31 / 39 | byte-identical |
| vanilla defaults, render scale 1 and 4, nursery / foyer | 39 / 31 each | byte-identical |
| 120 fps with `LM_INTERP_TEST=3` (fresh + in-betweens + late replays), user settings nursery and vanilla foyer, scale 2 | 73 / 62 | byte-identical |
| foyer turning (`turn` scenario), 120 fps, `LM_INTERP_TEST=4`, incremental vs `AURORA_ENH_MASK_FULL=1` | 362 | byte-identical |

The lit-room dumps first differed in the flashlight beam between any two runs (main against
itself too): the beam's flare advanced on wall-clock time. With the 0210 test aid both runs agree.

Light shadow maps changing shape in single frames (interp-final's turn scenario): in deterministic
`LM_INTERP_TEST=4` dumps of that scenario (frames 2600-2640, every in-between) the largest one-frame
artifacts are the same pixel for pixel with `extra_light_shadows=false` and with
`world_lights.flicker=false`, so they do not come from the light shadow maps there; the incremental
masks do not change any frame (table). What remains in those dumps is the flashlight pool and the
wall lighting next to the foyer candelabras stepping at the game frame. Not investigated further.

### Tried and not kept

- Rendering a cleared, 6-bit-resolved pass straight into the resolve scratch (saves the target
  copy, ~0.3 ms at 4K): the pass itself measured as much slower, no net change.
- Sharing the receivers' 16 mask taps between lights and AO (`var<private>` arrays): every enhanced
  EFB pass got slower (register pressure / occupancy).
- Restricting the 6-bit resolve to the union of the pass's draw rectangles: every LM pass with
  blended draws covers the whole target (3D view or full-screen quads), and pixels written raw
  (replace blends' alpha, dst alpha) make a partial resolve inexact anyway.

### Not done (bigger wins, outside this branch's scope)

- **Per-present replay.** Every pass above runs at the presented rate (4x per game frame at 120
  fps). Work that is the same for every in-between frame could run once per game frame: the
  game's EFB copies of content that does not move, and the light shadow-map atlas if its cube faces
  were world-aligned instead of view-aligned (now they turn with the camera, so the atlas changes
  every present even for static lights and casters; world-aligned faces would let a light's six
  faces be cached while it and the casters in its reach do not move). Both change sampling
  slightly (not byte-identical) or need the replay architecture (interp-final).
- The realistic flashlight's scattering is still 1.4-1.8 ms at Ultra/4K: per step it evaluates
  the reflector profile, two exponentials and the shadow test, plus a 9-point haze and a dust DDA
  (measured: march 1.0, haze 0.35, dust 0.33, shadow 0.2 ms). Fewer steps or a lower target are
  visible changes, so they were left to the quality presets.
- The 6-bit resolve (0176) could avoid its full-target copy only with in-place read-modify-write
  (framebuffer fetch / read-write storage on RGBA8), which WebGPU does not offer on D3D12.

## Original 30: render-worker cost of the per-draw early-Z split (Aurora 0229, issue #13)

Report (GitHub #13): at the "Original" frame rate (30, the default) cutscenes ran slower than real
time and the sound drifted ahead of the picture, most visibly in the opening when Luigi looks at
the map. At 31 fps and above it did not happen.

**Cause.** At 30 the game runs in lockstep: its frame is drawn straight into the Aurora frame and
`aurora_begin_frame` waits for the render worker. The early-Z split (0170/0179, "Early depth for
cut-out textures") is applied per draw at 30. When interpolating, the merged form (0192) is used,
but that one is not byte-identical. So at 30 every GX draw with early Z, a failing alpha test and
Z update becomes two commands, a depth-only half and a colour half. Each command set all of its
state again: `SetImmediates`, the uniform and texture bind groups, `SetIndexBuffer` and the blend
constant. The opening forest (alpha-tested foliage) had 18,000 split draws per frame, about
40,000 commands in all. Encoding took ~520 ms of worker time per second (measured with temporary
instrumentation), and Dawn's submit came on top of that. The worker needed 50-90 ms per game
frame on the test PC (other agents' builds and games running), and the game fell to 18-23 fps
(10-12 at worst). The opening took 72 s instead of 48.6 s, so the picture ended 23.6 s behind the
sound. The Parlor (behind the first key door) had the same cost and ran at 19-24 fps. Neither
keep-warm nor EFB-to-RAM copies were the cause (A/B: `LM_KEEP_WARM_CAP=0`, `LM_EFB_RAM=0`).

**Fix (0229).** `render_pass` encodes each run of GX draws with `gx::render_seq`. A state call is
issued only when it differs from what the previous GX draw set (immediates compared bytewise, the
uniform offset, the texture bind group, the blend constant). The whole index buffer stays bound,
and `firstIndex` selects each draw's range. Any other command type (custom draws, clears, other
draw types) resets the tracked state. A split pair then costs two `SetPipeline` and two draws
instead of twelve calls. `AURORA_GX_SEQ=0` restores the old path (A/B).

**Results** (1920x1080 window, default settings unless noted, guest frames per wall-clock second
from the `[frame]` lines, audio from `LM_AUDIO_LOG` + `LM_AUDIO_WAV`):

| | before | after |
|---|---|---|
| render worker per game frame, opening forest, busy PC | 50-90 ms | 11-15 ms |
| the same, quiet PC (6% CPU load) | 23-39 ms (29.48 fps, 0.8 s behind) | 8-12 ms (29.97) |
| opening (frames 883-2340), game fps | 19.6 (20.2 over the whole scene) | 29.97 |
| picture vs sound at the end of the opening | 23.6 s behind | +0.001 s (max 0.010 s within it) |
| opening with Enhanced lighting | | 29.97, drift -0.04 s |
| Parlor after the first key door | 19-24 fps | 29.94-29.99 |
| 60 / 120 / match display, opening | 29.97 (unchanged) | 29.97 / 29.94 / 29.96 |

Audio pushes stayed at the console rate the whole time (572 blocks of 560 samples per 10 s,
queue ~35.5 ms, no give-ups). The picture is unchanged: frame dumps (`LM_FRAME_DUMP`) of the
opening (frames 900-1500, every 20th) and of the key-door route (2950-4400, every 50th) are
byte-identical between `AURORA_GX_SEQ=0` and 1, with default settings and with Enhanced lighting.
(One Enhanced-lighting run of the old path differed from all the others, including a rerun of
the old path: the opening is not fully deterministic from run to run. Compare two runs per side.)

Test route for the key door (US): `LM_START_MAP=2 LM_START_APPEAR=6 LM_PATCHES=41,42` (Luigi
counts as having every key), `LM_SKIP_TEST=2700:pos:-850,290,-600` (on the left stairs), input
`2780:STICKRIGHT*60,2840:STICKUP+STICKRIGHT*40,2880:STICKUP*60,2960:STICKLEFT*30,3000:STICKUP*40,3050:A*3,3080:STICKUP*40`
after the usual menu input, with `LM_INPUT_CLOCK=frames`. The unlock close-up runs from guest
frame ~3061 to ~3901. During it the game logic ran at 60 fps on every build and frame rate
(issue #10). That is the heap corruption that the PAL-base work tracks, not pacing: in lockstep
the game waits for its own retrace message every frame.

**Found on the way (Aurora 0230).** While trying to reproduce #11 (Steam Deck), I ran the Linux build
in WSL. There Dawn falls back to OpenGL ES (llvmpipe) at the compatibility feature level, without
core limits. The GPU keep-warm compute shader (0215) used `@workgroup_size(256)`, above the
compatibility limit of 128. The first time keep-warm engaged, creating its pipeline was a fatal
uncaptured validation error (`[FATAL: aurora::gpu] ... workgroup_size(256, 1, 1) exceeds the maximum
allowed (128, 128, 64)`). It now uses 128 invocations per workgroup and 32,768 workgroups (the
same work), inside an error scope, so a failure turns keep-warm off. Devices with core limits
(D3D12, Vulkan on a real GPU) were not affected. On Windows keep-warm engages as before.

## Anti-aliasing sample counts (Aurora 0233, issue #14)

**Report** (v1.1, CachyOS, RX 7600 XT, RADV / Mesa 26.2.1, Vulkan): "after launching it when setting
AA to any number it does not launch anymore". Dawn: feature level Compatibility with
CoreFeaturesAndLimits enabled (so a core device; Aurora always requests the compatibility level
and enables core when the adapter has it). The log ended right after
`[DEBUG: aurora::gpu::cache] SQLite version`.

**Cause.** WebGPU defines MSAA sample counts 1 and 4. The F1 menu offered 2x/4x/8x, which worked on
Windows because Dawn's D3D12 backend hands any count to the driver (release builds run with
Dawn's `skip_validation`, so nothing rejected 2 or 8). Dawn's Vulkan (and Metal/GL) backend only
translates 1 and 4: creating the 2x/8x scene color target (`create_render_texture`, the first
thing after the Dawn cache opens and the surface is configured) hit Dawn's unreachable path.
Reproduced on Windows with `LM_BACKEND=vulkan` (RTX 3090 Ti, the same feature list as the report):
`abort()` inside webgpu_dawn.dll from `aurora::webgpu::create_render_texture` <- `initialize`, on
every start, so the menu could never be reached again to turn it off. On Linux the same abort, or
undefined behaviour where Dawn's release build compiles the unreachable out. 4x on Vulkan worked
(also with `AURORA_DAWN_VALIDATION=1`: no validation error through boot, title and foyer).

**Fix** (`patches/aurora/0233-msaa-supported-sample-counts.patch`, `src/recomp/menu.cpp`):
- Aurora decides the supported counts once the device exists: 1 always; 4 on any core device;
  2 and 8 also on D3D12. A compatibility-mode device without core features (OpenGL ES fallback)
  gets no MSAA, since the depth copies read the multisampled depth buffer, which compatibility
  mode does not allow. A saved count the device lacks becomes the highest supported one below
  it, with `[WARNING: aurora::gpu] MSAA 8x is not supported on this device (Vulkan); using 4x`.
  `settings.ini` keeps what the player chose (a D3D12 machine still gets 8x from the same file).
- `aurora_get_msaa_support()` (bit mask) / `aurora_get_msaa()` (count in use): F1 > Visuals >
  Anti-aliasing lists only the supported levels; a saved unsupported level shows
  "(8x not supported by this graphics device; 4x in use)". The level still applies at the next
  launch.
- The scene targets are created inside error scopes at initialization: if the multisampled ones
  fail (out of memory, a driver refusing them), the error is logged and the game continues
  without MSAA instead of running into a fatal error on the first frame.

**No silent hangs at startup** (`src/port/crash_handler.cpp`, `src/recomp/precache.cpp`, `main.cpp`,
`setup.cpp`):
- The hang watchdog is always on (LM_HANG_DUMP only adds the thread stacks): when the main thread
  has not come back to its loop for 15 s it logs `[hang] <t> s after start: main thread has not
  returned to its loop for 15 s (<phase>)` to stderr, then every 30 s, and once when it continues.
  Phases: graphics initialization (device, render targets, anti-aliasing), setup, loading the
  game, game loop, shutdown; a window paused in the background is not reported. The lines also go
  to `last_run.log`, and during a start the stall becomes the reason in `last_run_state.ini` that
  the recovery prompt shows after a killed start (`recovery::noteHang`, docs/SETUP.md "When the
  game does not start").
- The startup / settings hold reports a stall: with no pipeline finished for 20 s, `[precache] no
  pipeline finished for N s: P pending, T threads; creations running for over 10 s (graphics
  driver): config <hash> (type n) for N s` every 20 s (Aurora's in-flight table,
  `aurora_pipeline_cache_describe_stuck`); after 90 s without progress the hold ends ("gave up
  waiting ...: the game continues") and only a draw that needs a still pending pipeline waits
  for it. A pipeline whose creation fails never stalled the hold: `guarded_create` (0017) logs
  it, records the config as bad and counts it done. Test aid
  `AURORA_TEST_STUCK_PIPELINE=<n>[,<s>]` (the n-th creation takes s seconds longer): verified
  with n=50, 130 s: four stall lines naming the config, gave up at 90 s, the game ran on to the
  foyer.
- The crash journal (0231) is unaffected: an abort like the MSAA one is a crash before any
  pipeline is in flight (logged as a startup crash; caches reset only on the third in a row); a
  kill of a hung start is a kill (caches kept).

**Found on the way, not fixed:** with Enhanced lighting and a cache built in the same session (cold
start, about 10000 pipelines compiled), quitting (LM_QUIT_AFTER, also during the hold) took more than
90 s: the main thread is in `aurora_shutdown` freeing Dawn objects (the watchdog now names the
phase "shutdown"); even `Stop-Process -Force` took a while to end the process. Warm starts exit
at once.

**Verified** (US dev build, `--input "600:START*10,1000:A*6"` into the foyer, 170-240 s per run,
settings.ini with the level preset, scratch APPDATA, one shader cache per backend):

| Backend | AA saved | Enhanced lighting | Result |
|----|----|----|----|
| D3D12 (RTX 3090 Ti) | Off / 2x / 4x / 8x | off and on | each level as saved ("MSAA 2x/4x/8x"), game started, into the foyer (3180-5520 presents), no errors |
| Vulkan (same GPU, `LM_BACKEND=vulkan`) | Off / 4x | off and on | as saved, into the foyer |
| Vulkan | 2x / 8x | off and on | before: abort at start; now "not supported ... using no MSAA" / "using 4x", into the foyer |
| Vulkan + `AURORA_DAWN_VALIDATION=1` | 4x | off | no validation error, 7260 presents |
| WSL lavapipe Vulkan (`LM_ALLOW_CPU_GPU=1`) | 8x | off | "using 4x", game started; the unfixed 1.1.1 `LuigisMansionRecomp` with 2x there: abort right after "Using surface format", the report's symptom |
| WSL OpenGL ES (llvmpipe, compatibility) | 4x | off | "not supported (OpenGLES, compatibility mode); using no MSAA", game started (Dawn refuses `textureLoad` on depth textures and copies of multisampled textures in compatibility mode, both used by the MSAA depth copies) |

F1 > Visuals on Vulkan with 8x saved: the list is Off / 4x, 4x selected, note "(8x not supported by
this graphics device; 4x in use)". Not tested: a real AMD GPU / RADV (the report's device), Metal.

## Malformed FIFO data (issue #17, Aurora 0235)

**Report.** A player (version not stated, 1.1 or 1.1.1) got the fatal dialog `command_processor:
unknown opcode 0x1D at pos 11946`, and afterwards was "no longer able to launch the game at all".
0x1D is no GX opcode (0x1D & 0xF8 = 0x18). `pos` is the offset in the block the FIFO thread was
processing: everything published since it last stopped (at a `GX_BP_REG_DRAWDONE` or the end of
the previous block). So the bad byte sat 11946 bytes into a display list, a draw sequence or a
replayed tape (interpolation). Up to 1.1.1 any bad command stopped the game.

**What goes into the stream.** Aurora's own commands from the GX HLE (`hle_gx.cpp`), the game's
raw write-gather-pipe bytes (vertex data after `GXBegin`, inlined register writes; `hw::fifoWrite`),
display lists copied whole from guest memory (`GXCallDisplayList`), and with interpolation the
captured tapes, replayed by the host thread with blended values patched in (`interp.cpp`). A draw
whose vertex count does not match the bytes that follow, a list that is not what the game wrote,
or two threads appending at once all shift the stream. The next "opcode" is then data.

### Recovery (0235)

- Every command is checked before it is carried out: an unknown opcode, a draw with an impossible
  primitive or more vertex data than the block holds, a truncated BP/CP/XF/indexed load, an XF load
  past the end, a known Aurora subcommand that does not fit or carries an impossible draw, an
  indexed XF load from an unmapped array. Draws of 0 vertices and strips/fans of fewer than 3 are
  skipped (their index buffer size underflowed: an out-of-memory crash on garbage).
- The bad command is reported: `[ERROR: aurora::gx::fifo] FIFO: <what> at stream position N (offset
  O of a S-byte block); K bytes skipped, resynchronized | rest of the block dropped (error E)`, with
  the 64 bytes before it and the 32 from it on. The first 10 errors are logged, then at most one a
  second (with the count of those not shown).
- The port names the writer (`[fifo] malformed GX command (...)`, `fifoErrorReport` in
  `hle_gx.cpp`). For lockstep frames it uses a ring of the last 1024 GX call sites with the stream
  position each started at (`gx::noteCallSite`). For interpolated frames it uses the replay that put
  the tape in the stream (`interp::describeStreamPos`: game frame, tape, offset in it, the GX calls
  before it in the tape, with symbol names).
- Processing resumes at the next position where at least 8 plausible commands parse in a row
  (exact opcodes, known CP and XF addresses, sane counts, Aurora subcommand lengths; NOPs and empty
  draws do not count), or where commands parse to the exact end of the block. Otherwise the rest of
  the block (one publish: a draw, a display list call, a replay) is dropped, and the next block
  starts on a command boundary. A new Aurora subcommand must be added to `aurora_payload_len`, or
  resync never accepts it (processing it is unchanged).
- Only a stream that stays broken is fatal: `AURORA_FIFO_ERROR_LIMIT` errors (default 300) within
  10 s, with a message that says so. A bad tape is replayed several times (4 at 120 fps), so one
  bad frame is about 4 errors.
- Recordings (display lists, captures) stop at the first write that does not fit. They used to drop
  only that write and go on, leaving a list cut inside a command. `GXEndDisplayList` returns 0 for
  a list that overflowed, as the SDK does (`[gx] display list overflowed its N-byte buffer (begun
  at ...)`). Aurora's texture and array loads are longer than the console's, so a list sized for
  the console could overflow. None did in the routes below.

Test aids: `LM_FIFO_CORRUPT=<frame>[:byte|draw|junk|xf],...` writes malformed data into the FIFO
(or that frame's tape) at the end of a game frame; `AURORA_FIFO_ERROR_LIMIT=<n>`. Verified at 30
(lockstep) and 120 fps (tapes): all four kinds are reported with the right caller
(`LMChangeFrameBuffer__Fv`, `fn_8000783C__Fv`) and game frame, and the game goes on. With
`AURORA_FIFO_ERROR_LIMIT=3` and four bad frames in a row the fatal message comes, and the next start
offers recovery (docs/SETUP.md "When the game does not start").

### Guest threads and the FIFO

The console has one CPU FIFO that every thread writes to. Here each guest thread is a host thread
(only one runs guest code at a time). With interpolation the drawing thread records its frame into
a capture that belongs to its host thread, while the host thread appends replays to Aurora's FIFO.
LM's loader thread (`803A32A0` on the USA game) makes GX calls too: `GXInitTexObj`/`LOD` and
`GXGetTexBufferSize` at every map load (`fn_8005D41C` shadow setup, `fn_8005F0C4`,
`JUTTexture::initTexObj`) and `GXInvalidateTexAll` at boot (`fn_8003A430`, game frame 69, after
interpolation has started). Each of them used to run the whole GX entry on the loader thread:

- it closed the drawing thread's open draw (`gx::endPendingDraw`), although that thread may have
  been preempted between `GXBegin` and the end of its vertices. For a held 2D draw (widescreen)
  the held bytes were then replayed, and for any draw the next "raw write" state check emitted
  state commands, in the middle of that draw's vertex data;
- with interpolation, whatever it wrote went into Aurora's FIFO buffer directly, at the same time
  as the host thread's replays (an unsynchronized append from two threads).

Now (`hle_gx.cpp`, `interp.cpp`): functions that never touch the FIFO on the console
(`GXInitTexObj*`, `GXInitTlutObj`, `GXInitLight*`, `GXInitSpecularDir`, `GXGetTexBufferSize`,
`GXInitTexCacheRegion`, `GXInitFogAdjTable`) are `GXHLE_CPU`: they close no draw and are not call
sites. Other GX calls close only a draw their own thread opened (else `[gx] GX call ... while
thread X has a draw open; left open`). A guest thread other than the capturing one joins the
capture (`AuroraCaptureJoin`; `[interp] guest thread X joins the frame capture`), so its commands
land in the tape in the order the guest issued them. In a 400 s run at 120 fps through 14 map
warps (maps 1/2/3/9/10/11), 7100 GX calls came from the loader thread, none while a draw was open,
and no malformed command was reported. The race exists, but it was not caught causing #17.

### What was checked for "no longer able to launch"

- **Game module**: its key covers the program version, the module ABI version and hashes of ctx.h,
  module_abi.h, module_rt.cpp, the recompiler, hooks and symbol maps, and the DLL is renamed into
  place before the key is written (docs/SETUP.md). An update always rebuilds it, and the loader also
  checks `abiVersion` and `sizeof(RuntimeApi)`. Ctx, GameModule and RuntimeApi did not change in
  1.1 or 1.1.1.
- **settings.ini / install.ini**: parsed line by line; unknown keys are ignored and values clamped.
  A settings.ini with binary data (a cut-off write) is now put aside as `settings.ini.corrupt`.
- **Pipeline/shader caches**: they never feed the FIFO. Up to 1.1 every unclean end counted as a
  crash, and the next start deleted both caches and compiled everything again behind "Preparing
  shaders" (minutes with Enhanced lighting, easily taken for a hang; a kill there counted as another
  crash). Fixed in 1.1.1 (0231).
- **Memory card**: Aurora skips GCI files whose size does not match their header, and the game
  checks its own checksum. The 1.1 event01 bug decompressed 0x4E280 bytes to address 0: the OS
  globals and the start of `.text` (0x80003100-0x8004E280), which the recompiled code never reads.
  The save data comes from the game's own structures in the heap and `.bss`, so a save made on 1.1
  is not expected to carry the corruption (not verified with a 1.1 build).
- **Hidden Mansion overlay**: only read in the Hidden Mansion, after the file select.
- **Not reproduced**: boot to the foyer, the file select, 14 map warps, 5 to 7 minute sessions at
  120 fps, lockstep runs at 30 fps.

## Memory use over time (issue #34, branch `memory-leak`, Aurora 0250)

Report: memory grew from 1100 MB to 1900 MB in about 45 minutes, about 10 MB more when a room was
opened, enough to crash a low-RAM system. Three things grew: one leak in the port's GX HLE (the
gradual growth, on every setting), one per map change in Aurora (GPU memory), and one per shader
compiled (the biggest, a few GB after a cold start).

### Measuring

- **`[mem]` lines** in the log (last_run.log) every 60 s (`LM_MEM_LOG=<s>`, 0 = off;
  `src/recomp/mem_report.cpp`, written by its own low-priority thread since issue #43, see
  "The once-a-minute freeze" below): the process's private bytes, working set and peak, the bytes
  allocated in the C/C++ heaps only with `LM_MEM_HEAPS=1` or `LM_MEM_TRACK` (Windows `HeapSummary`,
  which stalls the game for most of a second; the rest of private is guest memory, thread stacks and
  the graphics driver's own allocations), the GPU memory the process uses (DXGI per-process
  local / non-local usage); then Aurora's cache sizes (`aurora_mem_stats`, 0250: live textures with
  bytes, texture object / content / TLUT caches, EFB copy textures and the copy-texture cache with
  bytes, bind groups, samplers, pipelines, shader modules, EFB copy-to-RAM entries and byte buffers)
  and the port's texture/TLUT object tables. A line that plateaus is a warm cache; one that keeps
  climbing is a leak. Linux: RSS / anonymous RSS from /proc; macOS: footprint.
- **`LM_MEM_TRACK=1`** (Windows, test aid, `src/recomp/mem_track.cpp`): every operator new of the
  runtime, Aurora and the static libraries is recorded with its call stack (global operator new and
  delete; one branch when off); each `[mem]` report lists the call sites whose live bytes grew most
  since the first report, symbolized from the PDB. **`LM_MEM_TRACK=2`**: every module's
  `HeapAlloc`/`HeapFree` imports are redirected instead (the C runtime's malloc, Dawn, the shader
  compiler, the graphics driver), also summed per module (the first caller outside ntdll, kernel32,
  the C runtime). Slow; for finding which module holds memory.
- Routes (scratch APPDATA with warm shader caches copied in, `LM_OFFSCREEN=1`, `LM_START_MAP=2
  LM_START_APPEAR=1 LM_TEST_STATE=poltergust LM_SKIP_CUTSCENES=1 LM_SKIP_AUTO=30
  LM_INPUT_CLOCK=frames`, A every 90 frames): **rooms** - `LM_SKIP_TEST` teleports every 150 frames
  through 8 mansion positions (`1800,0,-1400`, `-1600,0,-985`, `-2050,0,-900`, `3380,0,-220`,
  `-2210,0,-4150`, `0,550,-3300`, `-1648.7,550,-4889`, `-450,0,-5650`; the room number changes in
  `LM_DEV_STATUS_LOG`; avoid `-430,0,-10` and the roof `2402,2300,-2566`, where the game slowed to a
  crawl);
  **maps** - `LM_DEV_WARPS` every 450 frames through mansion appear points 0/6/2/1/4/8, the lab
  (map 1) and the training room (map 3); **idle** - no teleport (the control).

### 1. Host texture objects (the gradual growth)

The guest GXTexObj is smaller than Aurora's, so the HLE keeps the host objects in a table and the
guest object holds a tag and an index (`hle_gx.cpp` `ObjTable`). A new entry was made whenever the
guest object did not already hold a valid tag/index of its own address. J3D's `loadTexNo` (every
material texture of every model drawn, also `fn_8012B314`, `fn_80058C30`, the particle and 2D code)
initializes a GXTexObj on the stack, loads it and returns; the stack is overwritten before the next
call, so every call took a new entry: ~830 a second (game frames: the same at any frame rate),
64 + 4 bytes each plus deque growth, ~12 MB a minute, never freed (`LM_MEM_TRACK=1`: 256 000
entries, +16 MB at that one site in 5 minutes). That is the user's curve (800 MB in 45 minutes), and it
was the same idle as with teleports: the "room" in the report is only when it was noticed.

Fix: entries nothing refers to are reused. When the free list is empty and 16384 objects were
initialized since the last collection, guest memory is scanned for the tag: an entry is live while
any guest word pair (tag, index) names it (the object or a copy of it anywhere), every other one
goes on the free list. The scan covers only pages ever written (the guest memory is now allocated
with `MEM_WRITE_WATCH` on Windows, `runtime.cpp` `allocGuestMemory` / `guestWrittenRanges`; ~25 MB:
1.7-4 ms, every ~18 s of play; the whole 64 MiB window elsewhere). Host code never keeps a pointer
to an entry past the GX call that looked it up, and Aurora gets the object's values, not its
address (`GX_AURORA_LOAD_TEXOBJ`). The table now stays at ~16 600 entries (230-570 live).
`LM_OBJ_TABLE_LOG=1` logs each collection.

### 2. EFB copy textures of dead destinations (GPU memory per map change)

Aurora keeps a GPU texture per EFB copy destination (`copyTextures`) and a reuse cache of them per
(destination, size, format, replay generation) (`copyTextureCache`), at render resolution. Both were
dropped only when a texture later read that memory and found it overwritten. After a map change the
game's copy buffers (the shadow map, the GBH screens) land elsewhere in the heap and nothing reads
the old addresses again: at 120 fps + Enhanced lighting the copy cache went from 43 entries / 98 MB
to 75 / 248 MB and the process's GPU memory from 604 to 770 MB in 10 minutes of map warps, still
climbing (at 4K each 640x480 copy is ~11 MB per generation). With frame interpolation the lazy check
never ran at all (replays do not drop copies for CPU writes).

Fix (0250): every 64 frames, with the FIFO drained (`gx::texture::end_frame`), `efb_ram::sweep`
checks the tracked copies against memory as a texture read would (also during replays: the game is
ahead of them, and a copy it still uses is not written by the CPU); an overwritten one is dropped
and its copy textures evicted, also from the replay state slots (`sCopySlots`, `g_entrySlots`; the
next replay would have restored it). Copy-texture cache entries not copied into for 1800 frames go
(the texture a destination shows stays alive through `copyTextures`), beyond 128 destinations the
least recently copied are evicted (the bound with EFB copies to RAM off), and byte buffers no copy
uses are released. After: 14 minutes of map warps at 120 fps + Enhanced lighting, copy cache 13-22
entries (47-71 MB), 6-8 destinations, GPU local 550-580 MB, flat.

### 3. Shader modules compiled on a cache miss (GBs after a cold start)

0227 kept every GX shader module of the session for reuse. Dawn keeps a module's parsed shader
program (Tint, ~1 MB for a GX shader) while anything outside Dawn references the module, and only
builds it when it has to compile the shader (a module whose pipelines come from the Dawn cache
never parses it). So every shader compiled on a cache miss stayed in memory for good: after a cold
start with default settings Dawn held 2183 MB (`LM_MEM_TRACK=2`; warm start: 88 MB), private 4.4 GB
instead of 2.0; with Enhanced lighting at 120 fps private was 11.1 GB (peak working set 10.6 GB),
which a low-RAM system cannot survive, the compile took 254 s, and the exit afterwards 127 s
(Dawn freeing them: the "exit after a cold Enhanced-lighting compile takes 15-90 s" note). Each
pipeline compiled during play (a cache miss: new room, new effect) added its module too.

Fix (0250): the module cache keeps the latest 384 modules (a shader's variants are queued
together; reuse 1895 of 10076 pipelines instead of ~2500) and is released whenever no pipeline is
left to build (`begin_pipeline_frame`). After, cold start: default settings private 2.16 GB (Dawn
90 MB); Enhanced lighting at 120 fps private 2.9 GB, peak working set 2.5 GB, compile 105 s,
exit immediate. Warm starts are unchanged or faster (default 0.32 s; Enhanced lighting 1.0 s for
10082 pipelines, 1.75 s before).

### What plateaus (not leaks)

The pipelines themselves (3055 default, 10082 Enhanced lighting: the driver's ~150-450 MB), the
interpolation vertex-snapshot arena (3 generations of 1 MB chunks, grows to the peak need), the
display-list parse cache (cleared at 50 000 entries), Aurora's texture object cache (600 frames
idle, 15-130k entries at ~60 B), the texture content cache (128 MB budget), nod's disc cache.

### Before / after (default settings, 30 fps, rooms route, warm caches)

| Minutes | 1 | 5 | 10 | 13.5 | 29 |
|---|---|---|---|---|---|
| Before: private MB | 1924 | 1991 | 2037 | 2062 | (+12 MB/min) |
| After: private MB | 1915 | 1933 | 1932 | 1931 | 1938 |

After: heaps 476-488 MB, GPU local 430-434 MB, texture object table 16 623 entries over the 29
minutes (~350 room changes). Idle control before: 1929 -> 2037 MB in 9.5 minutes (the same slope as
with teleports). Enhanced lighting at 120 fps, maps route, 14 minutes after: private 2878-2991 MB
(per-module heap use flat, `LM_MEM_TRACK=2`). Frame dumps
at Original 30 (key-door route of "Original 30", `LM_DVD_SPEED=1`, frames 2950-4400 every 50th):
all 30 byte-identical between the build before and after. (A second run of the old build differs
from the first in 13 frames of that route, late in it: the route is not fully deterministic, so a
single identical pair is the stronger result.)

### The once-a-minute freeze (issue #43, branch `issue43-stutter`)

Report (1.1.5 only; 1.1.4 fine): every 60-90 s the game froze for most of a second, sound included
(3-4 stutters, the FPS counter down to 3-32 with "PC too slow"), then went on; not tied to anything
the player did. Reporters at 3440x1440 165 Hz and 4K 160 Hz with Enhanced lighting.

Cause: the `[mem]` report added in 1.1.5 for issue #34. It ran on the host (frame) thread every
60 s, and its C/C++ heap total used `HeapSummary`, which walks every block of each process heap
while holding that heap's lock: **835-880 ms** here (Enhanced lighting Ultra, 4K, heaps ~1.1 GB;
the reporters' heaps were 1.4 GB). The host thread presented nothing meanwhile, and every other
thread that allocated (render worker, Dawn, the driver, the guest's HLE calls) waited on the heap
lock, so the guest and its audio mix stopped too (`[audio] device queue ran dry`). The reporters'
logs show it exactly: one present interval of 400-970 ms right after each `[mem]` line, `hitches 1`
in every `[perf]` minute, `render worker ... max 430-437 ms`.

Fix: the heap total is no longer taken by default (only with `LM_MEM_HEAPS=1` or `LM_MEM_TRACK`,
knowing it stalls); the report runs on its own low-priority thread (the host thread only wakes it);
the DXGI adapters for the GPU memory figures are enumerated once instead of every report. A report
that takes over 20 ms says so (`[mem] report took N ms (sampler thread)`; the first one can, while
DXGI loads).

Measured (lmrecomp, 4K window, Enhanced lighting Ultra settings, `mixed` pacing scenario; the
machine was shared with several other game instances, so the frame rates themselves are low and
noisy): before, intervals of 849 / 914 / 971 / 960 / 912 ms within 0.3 s of each of the five
`[mem]` reports (run p99 58 ms); after, at 165 fps for 11 minutes the longest interval within 3 s
of each of the eleven reports is 56-183 ms, below or near that run's p99 (133 ms), and no interval
over 300 ms anywhere near a report; at 120 fps for 11 minutes 16-158 ms (p99 115 ms). Private bytes
flat (3555-3598 MB at 165, 3616-3727 MB at 120), so the #34 fixes are unchanged. The other periodic
work new in 1.1.5 is small: the GXTexObj table collection (every ~19 s, `LM_OBJ_TABLE_LOG=1`) takes
9-22 ms on the guest thread (inside a 33 ms game frame); Aurora's copy-texture sweep (every 64 frames)
was not measured separately, and neither shows as a stall in the pacing logs.
