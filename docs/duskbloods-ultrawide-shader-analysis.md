# The Duskbloods ultrawide shader analysis

Network test title ID `0x0400D1D02399A000`. Lichii checkout `feat/duskbloods-ultrawide` at `a6ab7c4aa3a99bc2dda8461e49a050a5468acbe9`. This is a static investigation of existing captures and local replay data.

## Result

**Genuine 3440×1440 rendering is plausible and unverified.** The captured world shaders apply supplied perspective matrices. One recovered camera matrix encodes 16:9, with jitter matching the active 888×500 scene rectangle. A separate 16:9 factor in the camera-motion pass also comes from a bound buffer. The trace establishes distinct active scene, reconstruction, backing and presentation extents. Every observed scene and reconstruction pair stays at or close to 16:9, so it does not establish that the game can choose a different aspect.

The strongest next step is to trace the guest's camera matrix and render-extent producers in Ghidra. A useful first experiment needs a wider camera frustum and matching culling, image allocations, GPU render state and reconstruction constants. Changing the window aspect or enlarging the present crop alone cannot expose additional world geometry.

## 1. Capture inventory and provenance

Primary capture root `C:\Users\KrNikitin\DuskCaptures\render-audit-good`. All reads were local. The [native inventory](#appendix-native-program-inventory) lists every program key, its stage and all recorded addresses.

| Path or file class | Count | Bytes | Contents |
|---|---:|---:|---|
| `shaders/*-gfx-*.bin`, excluding sidecars | 411 | 2,299,712 | Graphics instruction streams, 80–56,528 bytes each |
| `shaders/*-cs-*.bin` | 44 | 389,312 | Compute instruction streams, 144–47,872 bytes each |
| `shaders/*.header.bin` | 411 | 52,608 | Exactly one 128-byte sidecar per graphics file |
| `frame/*.png` | 419 | 153,904,873 | Image previews across surviving capture material |
| `frame/*.raw.zst` | 431 | See combined count below | Compressed image data |
| `frame/buffer_*.bin.zst` | 1,773 | See combined count below | Compressed buffer contents |
| `frame/*.zst` combined | 2,204 | 1,027,958,008 | Image and buffer data |
| `frame/manifest.jsonl` | 1 | 2,578 | Frame 30 at 3.889 s, three clear-only target records, two present records and a summary |
| `frame/index.html` | 1 | 275,509 | Older, partial index for frame 3180 at 112.69 s |
| `frame/capture.now` | 1 | 0 | Capture trigger marker |

There are 3,492 files totaling 1,184,882,600 bytes. There are no standalone `.env` records, serialized pipeline caches, logs, SPIR-V modules or RenderDoc captures inside this root. The capture has 455 native files and 401 distinct native keys. The 54 extra files record repeated code at different GPU program addresses.

| Stage | Files | Unique keys | How established |
|---|---:|---:|---|
| Vertex | 137 | 101 | Header type 1 and matching cache stage |
| Tessellation control | 8 | 8 | Header type 2 and matching cache stage |
| Tessellation evaluation | 16 | 14 | Header type 3 and matching cache stage |
| Fragment | 250 | 234 | Header type 5 and matching cache stage |
| Compute | 44 | 44 | `-cs-` name, no graphics sidecar, matching compute environments |

The graphics sidecars are conventional envelopes in this capture. Their `(sph_type, shader_type)` pairs are `(1,1)`, `(1,2)`, `(1,3)` and `(2,5)`. Shader type alone can be ambiguous for task/mesh envelopes in general. The matched runtime cache confirms the stages above. It supplies no evidence of task, mesh or geometry programs in this set.

The current JSONL summary has **zero draws and zero dispatches**. All its 3840×2160 image entries have `rescaled: true` and flat black RGB. It is a startup presentation and cannot classify gameplay shaders. The HTML names a different frame, contains 436 cards (50 pass, 43 dispatch, 343 sampled), and reports an unfinished capture. All 90 shader hashes in it could be resolved. Exactly 224 cards have a same-name raw image still on disk and 212 have a same-name PNG. Other images and buffers survive outside the index. An image, buffer filename or timestamp alone cannot establish a binding, draw or common frame. The HTML shows gameplay through draw 270 and has no present card.

Two supplementary local sources filled specific gaps.

- `%APPDATA%\eden\cache\shader\0400d1d02399a000\vulkan.fork-v56.bin` contains 14,412,063 bytes, 851 pipeline records and 1,766 serialized shader environments (807 vertex, 773 fragment, 71 tessellation control, 71 tessellation evaluation, 44 compute). It is newer than the primary capture. Exact packed-code matches establish stage and reusable metadata across runs. Specialization records do not supply general live uniform contents or establish when a draw ran.
- `C:\Users\KrNikitin\DuskCaptures\render-audit\frame\manifest.jsonl` is a **separate** capture of frame 3330 at 117.894 s. Its summary records 460 draws, 64 dispatches and truncation. It retains buffer bindings and launches through late draws, with no present record. It shares 82 shader hashes with the primary HTML. Its manifest and named Zstandard blobs establish the numerical buffer observations below for frame 3330. They cannot relabel the primary frame's orphan buffers. Only 132 buffer filenames from the sibling manifest exist in both roots.
- `%APPDATA%\eden\log\eden_log.txt` records a later `fix/duskbloods-upscale-windows-606185116d` build and the requested title ID. Native-key-to-shader-hash pairs in this log cover 315 primary native keys and 88 of the primary HTML's 90 shader hashes. It separately records the working 1108×624 → 1920×1080 replacement and a 1920×1080 docked VI2 crop. It is independent corroboration, with no assumption of frame identity.

## 2. Decoding and replay

- The native files consist of 16-byte, little-endian Ampere instructions. `NativeProgramKey` hashes those bytes with FNV-1a. I recomputed all 455 filename keys. The filename address is the guest GPU program base. A graphics instruction at file offset `o` came from `base + 0x80 + o`. For compute it came from `base + o`. Offsets in this report are **native instruction file byte offsets** unless explicitly labeled as buffer offsets.
- Graphics header stage comes from common word 0, bits 10–13, then is checked against the cache. A small scratch driver used the checkout's `Instruction::Bits` and `LowerToMaxwell` with the captured stage, native code address and tensor support. All 455 streams lowered. None required accepting an unverified trap. Every packed result matched a local serialized environment after stripping the internal header, Maxwell scheduling words, padding and native address map. Cache stage and pipeline-key slots supplied the renderer shader hashes. Multiple cache records can share one packed shader. A filename key and renderer hash are different identifiers. [Capture and serialization](../src/video_core/shader_environment.cpp), [Ampere decoder and lowering](../src/shader_recompiler/frontend/ampere/lower_to_maxwell.h), [program key](../src/shader_recompiler/frontend/ampere/program_override.h), [pipeline cache](../src/video_core/renderer_vulkan/vk_pipeline_cache.cpp).
- The supplied Python sweep recognizes cache versions through v53. For this analysis, a scratch invocation admitted v56 after checking the field sequence against `GenericEnvironment::Serialize`, `FileEnvironment::Deserialize` and the Vulkan keys. It parsed to exact EOF. For targeted replay I linked the existing `shader-sweep` sources against the locally built recompiler and dependencies. The CLI used its v53 **environment-record reader** on extracted v56 records whose fields and key sizes matched. The input to this step was the extracted lowered environment, with the captured graphics key for graphics and captured workgroup/shared-memory metadata for compute. Ten selected records emitted SPIR-V, passed `spirv-val --target-env vulkan1.2`, and were disassembled with `spirv-dis`. These were renderer hashes `af80c123fbd8d4ef`, `ef7367d39cb6b7ab`, `874648f8d5c0faca`, `4f240c3d44ea9f5e`, `5bbb66bb76b0d041`, `e4d02338e0e3b79c`, `c23ee2b7ea37480d`, `513325577caa8b33`, `6148b6ff87c55d4f` and `cc79ae5887e4857d`. The replay confirms such features as the position builtin, sampling, storage writes and compute local sizes under the offline profile. It omits live pipeline linkage and device execution. [Sweep runner](../tools/nx2/shader_sweep.py), [sweep guide](../tools/nx2/shader-sweep.md), [offline compiler](../tools/shader-sweep/README.md).
- I used the repository's opcode and operand definitions to decode native attribute, ALU, uniform, memory, texture and control operands around the relevant instructions. For example, native `AST a[0x70]` through `a[0x7c]` are XYZW position stores. `LDG` can fetch matrix rows through a 64-bit pointer loaded from a constant buffer. Lowered Maxwell uses scratch registers and can expand a single native instruction. Generated SPIR-V uses translated descriptors and IDs. Neither representation supplies original native offsets without the mapping. [Attribute definitions](../src/shader_recompiler/frontend/ir/attribute.h), [native address map](../src/shader_recompiler/frontend/ampere/program_address_map.h).
- `nvdisasm` and `cuobjdump` were absent from PATH, and I found no `nvdisasm` in the checked NVIDIA toolkit/install and Vulkan SDK locations. Vulkan SDK 1.4.357.0 supplied the SPIR-V utilities. NVIDIA documents raw-instruction mode for a specified SM architecture. A future check would need a verified architecture choice and confirmed graphics-opcode coverage. The Lichii decoder already resolved the relevant paths here. No dependencies were installed. [NVIDIA CUDA Binary Utilities](https://docs.nvidia.com/cuda/cuda-binary-utilities/index.html#command-line-options).

## 3. Camera and clip-space dataflow

In primary frame 3180, pass 12 draws 123 times into six 1920×1080 color targets including R16G16 float at GPU address `0x81625de000`. The pass 12 albedo and packed target previews visibly contain scene geometry and a character. The populated region in those images is the top-left 888×500. The pass groups multiple shader hashes, so the individual draw binding comes from frame 3330 or the cache where specified.

| Native file | Renderer hash / stage | Native offsets and evidence |
|---|---|---|
| `8f119e6f3610f754-gfx-2312a71700.bin` | `af80c123fbd8d4ef`, vertex | `0x70` loads a three-component vertex position, `0x170–0x190` fetch object transform data, `0x1a0–0x270` form an origin-relative position. `0x0f0` loads the camera block pointer from `c4+0x20`. `0x110–0x160` fetch the four rows via that pointer at `+0x180/+0x190/+0x1a0/+0x1b0` and the origin at `+0x30` (`0x100`). `0x280–0x380` apply the supplied coefficients to XYZW. `0x390` writes all four position components. Pass 12 and frame 3330 draw 16 identify this as a world/depth position path. |
| `ac87a3a41e4ffc33-gfx-2320bd1400.bin` | `ef7367d39cb6b7ab`, vertex | Pass 12 world/material variant. `0x130–0x150` load external pointers `c4+0x18` and `c4+0x20`. `0x160–0x1d0` load origin and the same four clip rows from the latter. `0x1e0–0x200` fetch per-object transform rows. `0x6f0–0x730` emit varyings, including XYZW at `0x710` and a copy at `0x720`. An extra matrix/data path feeds varyings. The semantic role of every varying is unresolved. |
| `ae1504bb159dc949-gfx-2312710800.bin` | `874648f8d5c0faca`, vertex | World/effect geometry in pass 42 and frame 3330 draw 306. `0x00` loads XYZ. `0x10–0x100` apply `c4[0x00..0x3c]` to `(x,y,z,1)`. `0x110–0x200` apply `c3[0x80..0xbc]` to the resulting four components. `0x210` exports the intermediate and `0x220` writes clip XYZW. Bound data confirms an affine object matrix and a perspective camera-family matrix. |
| `b70394e7a665f103-gfx-2312702400.bin` | `4f240c3d44ea9f5e`, vertex | Pass 31. `0x00` loads XYZ, `0x50–0x140` compute `X*c3[0x00..0x0c] + Y*c3[0x10..0x1c] + Z*c3[0x20..0x2c] + c3[0x30..0x3c]` as four components. `0x150–0x180` export four passthrough attributes and `0x190` writes XYZW. This is a supplied transform in a scene/effect pass. Its exact draw role is less certain than the first two rows. |

For the first shader, frame 3330 draw 16 identifies vertex uniform slot 4 as `buffer_f7aa16f2880e72bc.bin.zst`. Its 64-bit value at `+0x20` is `0x813b7b0000`. This matches the same draw's storage binding 2 GPU address, with contents in `buffer_fc5d1f6f3516a860.bin.zst`. Reading those bytes as four vectors reproduces the clip rows used by the native shader:

| Storage byte offset | Clip component | Float values, rounded |
|---|---|---|
| `+0x180` | X | `(-1.236484, -0.00003572, 0.714327, 0)` |
| `+0x190` | Y | `(0.0636874, 2.535448, 0.110367, 0)` |
| `+0x1a0` | Z | `(0.000003987, -0.000000405, 0.000006909, 0.08000064)` |
| `+0x1b0` | W | `(-0.499162, 0.0507474, -0.865021, 0)` |

W depends on the origin-relative XYZ and has approximately unit spatial norm. Removing the X/Y projections onto the W direction gives an X-to-Y spatial norm ratio of **0.56250003**, matching `9/16`. The small W-direction coefficients of X and Y are approximately `-0.00070375` and `+0.00140742`. The same frame's prepass jitter `(-0.3125, -0.3518519)` gives `2*jitterX/888 ≈ -0.00070383` and `-2*jitterY/500 ≈ +0.00140741`. This is consistent with a jittered perspective projection for the main camera. For the effect shader, the draw-306 slot-3 buffer `buffer_3a0faae675958031.bin.zst` carries closely related spatial coefficients and the same W direction at `+0x80..0xbc`. The slot-4 buffer `buffer_219c1ea932e2bc9d.bin.zst` carries the affine transform.

These shaders consume projection coefficients from guest-bound data. Their position paths have no aspect division or embedded 16:9 coefficient. The data is compatible with a different horizontal coefficient and consistent perspective W. The capture cannot locate the ARM64 producer, distinguish a fixed display-mode table from an aspect calculation, or prove that culling and previous-frame matrices would follow a changed coefficient.

## 4. Screen-space and resolution assumptions

| Native file | Renderer hash / stage | Native offsets and interpretation |
|---|---|---|
| `7228ab2a96ab75c4-gfx-2312713a00.bin` | `5bbb66bb76b0d041`, vertex | `0x00–0x50` read attributes, `0x60–0x90` copy XYZW to position, and `0xa0–0xb0` copy UV-like inputs to varyings. The cache replay emits a position builtin and no cbuf for this program. Used for pass 13 and many single-draw screen passes across different extents. Rectangle coverage can be determined by the vertex data and viewport. |
| `ca4beafaad0f3d62-gfx-231294a100.bin` | `e4d02338e0e3b79c`, fragment | Pass 13 writes the R16G16 motion target at `0x81625de000`. `0x00–0x20` fetch coordinate factors through `c7+0x10`, `0x30–0xe0` prepare interpolated coordinates, and `0xf0` samples the bound depth input. `0x100–0x1d0` use a cbuf matrix and reciprocal, `0x1e0–0x1f0` form coordinate differences, `0x210–0x240` apply output scale/bias. Sibling draw 186's `buffer_c148b7423d737d3e.bin.zst` contains **16/9 at `c3+0x40`**, applied to X at `0x210`. Y uses `c3+0x44 = 1` at `0x220`. This is confirmed aspect-dependent motion encoding from uniform data. |
| `00fe7d8956d5cb68-gfx-2313cb2e00.bin` | `c23ee2b7ea37480d`, fragment | Sibling passes 82–88 reuse it on targets from 480×270 down to 16×5, with intermediate padded sizes such as 128×68. `0x30–0x50` read interpolants, `0x150` loads a pair of kernel offsets, `0x160–0x170` multiply by `c3+0x10/+0x14` and add to sample coordinates, `0x200` samples, `0x210` fetches a weight. The same kernel works at several extents using bound sample transforms. |
| `207b94e565848897-gfx-2313ceb000.bin` | `513325577caa8b33`, fragment | Sibling pass 116, draw 415, produces a color-managed scene image before the late UI-like draws. `0x00–0x20` read W and UV-like varyings. `0x110–0x220` use `c4+0x1b0..0x1bc` to scale/adjust sampling. The bound `buffer_c36a898402c7f792.bin.zst` contains `(0.8, 0.8, 1/240, 1/135)` there. `0x300–0x310` sample further inputs and `0x570` onward processes color. `0x560` is a branch with a displacement of 1920 bytes. |

The corpus scan covered 401 distinct native streams. I screened the 32-bit operand word for the integer and single-precision representations of 1920, 1080, 2560, 1440, 3440, 1108 and 624, integer last coordinates 1919/1079, float 16/9 and 9/16, selected reciprocals including 1/1920 and 1/1080, and packed 16-bit width/height pairs for 1920×1080 and 2560×1440 in either order. The 47 exact raw-word hits reduce to 17 branch displacements, 14 reconvergence operands, 15 cbuf operands and one actual integer immediate. In particular, `0xa00` at offset `0x00` in either DLSS kernel encodes `c0+0x28`. In compute `2abce208c834fd96-cs-23127eb500.bin` (renderer hash `7b1a701782722354`), the 1440 at `0x2ca0` is a `LEA` address addend. The result goes to a warp match and shared atomic at `0x2cb0–0x2cc0`, beside a parallel `0x5c0` addend at `0x2da0`. It is a shared-memory layout constant. No exact full-word hit establishes a fixed pixel extent or projection.

These checks are a filter, with dataflow taking precedence over numerical matches. An aspect can be formed by arithmetic on smaller constants, use a different immediate encoding, reside in a resource or be computed before GPU submission. The confirmed 16:9 values above came from bound data. The tessellation files establish stage coverage and contribute to the literal scan. Their full output dataflow and CPU patch/culling path were outside the targeted traces.

Render-target allocation, viewport, scissor and shader sample domains are separate. Lichii's `RasterizerVulkan::UpdateViewportsState` reads viewport registers or falls back to `surface_clip`. `UpdateScissorsState` reads guest scissors and applies emulator resolution scaling. Shader stores and the backing dimensions in an image manifest cannot recover the per-draw values of those registers. The primary frame's 888×500 occupied region is consistent with the sibling's measured active extent. [Viewport and scissor translation](../src/video_core/renderer_vulkan/vk_rasterizer.cpp).

## 5. UI, compositor and presentation

The sibling trace retains late draw bindings after the scene image at draw 415. Draw 417 has both stages below and follows a 1024×1024 clear at pass 118. The repeated late fragment variants and samples from a 1×1 image and small RGBA8 textures (16×42 and 14×42 among them) support an overlay/UI classification. Truncation removes the corresponding final targets and present image. The assets and shader patterns do not identify individual HUD elements or menu screens.

| Native file | Renderer hash / stage | Native offsets and evidence |
|---|---|---|
| `5a9883279a0a2653-gfx-23134ffc00.bin` | `23a62093d6a1390e`, vertex | Draw 417. `0x00–0xb0` read pane/mesh data and derive a matrix index from an attribute. `0xc0–0x1f0` load the selected `c3` rows at indexed offsets in `0x00..0x38`. The ALU applies them before writing clip XYZW at `0x310`. Outputs include texture coordinates and per-vertex data. |
| `06f24823640f85c5-gfx-2313cff100.bin` | `63925dc06cda2ec9`, vertex | Draw 423. `0x60–0xc0` derive an index from vertex data. `0xe0–0x190` fetch indexed `c3` rows. `0x270` writes clip XYZW. Both UI candidates bind the same 64 KiB slot-3 buffer `buffer_266eb9c3a7a8b89d.bin.zst`. Matrix records at `+0x80/+0xc0` include X/Y coefficients `1/1920` and `1/1080`. The actual per-vertex matrix indices and the CPU layout space remain unknown. |
| `c9e6784cf53404f8-gfx-23134fb500.bin` | `b87fd323537c09ea`, fragment | Draw 417. `0x110–0x120` interpolate UV-like data, `0x190` samples a texture, and `0x340–0x3a0` use indexed `c3` data and a second sample. This pairs with the first UI vertex candidate. |
| `78eef89a9ad6ace2-gfx-23134f7100.bin`, `b7a5d627e74b6416-gfx-23134f8500.bin` | `87ab1fa1605501ad`, `cbeda2ce5f5a8124`, fragments | Repeat in draws 418–456. Examples include interpolation and indexed material constants at `0x00–0x100` and texture sampling at `0x360` in the first variant, and interpolation plus indexed constants at `0x130–0x250` in the second. These paths are candidates for the overlay effects, with no evidence of a final whole-screen aspect mask. |

A centered 16:9 UI region is plausible if the scene and overlay submit separately and the latter can keep its original logical canvas. At 3440×1440, a 2560×1440 UI destination occupies `x=[440,3000)` at full height. Preserve the full scene destination, use this region for overlay transforms/scissors and verify anchor placement. If the game bakes the HUD into the scene prior to reconstruction, or uses full-screen overlays for both UI and post-effects, verify those paths before applying a shared UI rectangle.

There is **no confirmed guest shader mask or final guest crop** in the gameplay records. The 888×500 region in primary intermediate images starts at the origin and can be surrounded by black, white or other clear colors, depending on the target. Likewise, the sibling's output occupies 1536×864 of a 1920×1080 backing. These are partial image writes and the final gameplay present was not captured.

Lichii can crop and add bars after guest rendering. `Nx2Gsv::Composite` decodes the guest present's packed last column/row (`shown_size`) and clamps its use to the actual layout. The later log records `0x0437077f`, which decodes to 1920×1080 and yields a full 1920×1080 crop in docked mode. Handheld mode caps the crop at 1920×1080. `EDEN_NX2_CROP_WIDTH` and `EDEN_NX2_CROP_HEIGHT` override that host crop when both are set, so they cannot establish a wider guest allocation or wider FOV. `NormalizeCrop` and the Vulkan layer sample the selected source region. The frontend's `R16_9` fits and centers a 16:9 destination in the window, while `Stretch` fits the actual window ratio. On a 3440×1440 window, 16:9 fitting leaves 440-pixel side margins. The nominal `R21_9` ratio (21/9) also differs from 3440/1440 (43/18). For a correctly composed 3440×1440 guest image, match the host destination to that actual ratio. [VI2 composition](../src/core/hle/service/nx2_gsv.h#L2112), [source crop](../src/video_core/framebuffer_config.cpp#L33), [Vulkan layer](../src/video_core/renderer_vulkan/present/layer.cpp), [destination layout](../src/core/frontend/framebuffer_layout.cpp), [window clear and composition](../src/video_core/renderer_vulkan/present/window_adapt_pass.cpp).

## 6. DLSS and dynamic resolution

Both known Duskbloods keys are compute kernels identified by exact title and native key in `CapturedPasses`. In sibling frame 3330 they bracket dispatches 48–60, with no intervening graphics draw. Dispatches 49–59 use nine other captured native compute programs and changing grid sizes. That is the observed reconstruction interval. Individual internal network layers and the data dependencies of every dispatch were not traced. [Kernel identities and constants](../src/video_core/fire_emblem_upscale.h), [dispatch interception](../src/video_core/engines/kepler_compute.cpp).

| Native file | Renderer hash / stage | Native instruction evidence |
|---|---|---|
| `e5efe04581720f15-cs-21e9e53000.bin` | `6148b6ff87c55d4f`, compute prepass | `0x00b0/0x00e0` use coordinate scales `c0+0x1d8/+0x1dc`. `0x00d0/0x0100` add jitter `c0+0x1b0/+0x1b4`. `0x04f0` loads the depth handle at `+0x288`, `0x16d0` loads motion at `+0x284`, and `0x23d0` loads the color handle at `+0x278` alongside the next word. `0x1f50–0x1fc0` add per-axis motion bias `+0x1a0/+0x1a4` and apply multipliers `+0x198/+0x19c`. `0x2e50/0x2f00` convert render height/width at `+0x1ec/+0x1e8` to float. |
| `50e47e31c31fc1f8-cs-21ec8ae000.bin` | `cc79ae5887e4857d`, compute output | `0x1510–0x1540` use `c0+0x394/+0x39c` as clamp operands for source texel coordinates. `0x15a0/0x15b0` fetch color through `+0x328`. `0x1d20/0x1db0` and `0x6310/0x6330` compare against output height/width at `+0x358/+0x35c`. The later predicated `SUST` sequence at `0x6e00–0x6e90` writes through the `c0+0x348` descriptor. Other stores target other surfaces. The kernel has separate source and destination bounds and receives both from the guest. |

| Evidence | Active scene | Active reconstruction output | Backing or present |
|---|---|---|---|
| Primary frame 3180 | Occupied extent 888×500 in main scene previews | Output pass outside surviving index | 1920×1080 color/motion and 960×540, 480×270 or smaller intermediate images |
| Sibling frame 3330 | 888×500 in `buffer_180a8b73acf4033c.bin.zst` at `+0x1e8/+0x1ec`. Output buffer last column/row are 887/499 | 1536×864 in `buffer_e2bc896ce89f1a00.bin.zst` at `+0x35c/+0x358`. The output image's populated region agrees | Prepass intermediates at 1536×864. Output stored in 1920×1080 B10G11R11. Final presentation absent |
| Later successful DLSS log | 1108×624 | 1920×1080 | 1920×1080 backing color, motion, depth and output. Docked present crop separately logged as 1920×1080 |

Sibling prepass grid/block is `(96,54,1)/(128,1,1)`, with 1,664 shared bytes in the cache. Output grid/block is `(24,27,1)/(256,1,1)`, with 16,384 shared bytes. Scene color is B10G11R11, scene motion is R16G16 float, and the title's input depth is D24S8. The sibling prepass values `(0.578125, 0.5787037)` at `+0x1d8/+0x1dc` equal `(888/1536, 500/864)`. Its motion multipliers are `(-864,-864)` with zero bias. This gives per-axis output-normalized factors `(-0.5625,-1)` under the current replacement's convention. The varying scene/output/backing extents and the uniform-driven native bounds show genuine subrect rendering and reconstruction. They give evidence of distinct controls, with their upstream coupling and allowed aspect combinations still unknown.

The existing replacement reads both regions from the guest. `ValidRegions` checks each axis against its backing and requires render extent to fit within output extent. The hook requires the same current scene inputs and prepass/output agreement. The later log shows that it rejects a transient mismatch and then records a successful 1108×624 → 1920×1080 output when the extents agree. The texture-cache path validates the formats and fits, converts the motion convention, runs DLSS or FSR 3.1, and draws the result into the requested extent. Streamline tags zero-origin render and output subrects, passes render-pixel jitter, supplies `renderWidth/renderHeight` for its aspect field, and checks both dimensions against supported mode ranges. Its first mode guess is based on width ratio. Output size and mode changes reset history. Unsupported DLSS mode extents can fall back to FSR, while a rejected guest contract does not yield an enlarged replacement output. [Region checks](../src/video_core/dlss_contract.h), [guest region and motion conversion](../src/video_core/engines/kepler_compute.cpp#L1208), [Vulkan handoff](../src/video_core/renderer_vulkan/vk_texture_cache.cpp#L1770), [Streamline sizing and history](../src/video_core/renderer_vulkan/vk_streamline_dlss.cpp#L279), [current contract](native-dlss-replacement.md).

Consequently, setting only the DLSS output width would leave the 16:9 world frustum and existing source sampling domain in place. 3440×1440 also exceeds the captured 1920×1080 output backing. An aspect-changing experiment must coordinate guest viewport and sample scales, both reconstruction cbufs, output and history allocations, dispatch coverage, depth and motion, and downstream consumers. Structural SPIR-V validation does not establish the native network's tiling or resource safety at a new extent.

## 7. Hypotheses

| Hypothesis | Result |
|---|---|
| **A. The engine is largely resolution-independent** | Supported for the examined shader interfaces. World clip position consumes supplied camera coefficients. Screen-space filters reuse kernels at different dimensions. Reconstruction consumes separate per-axis extents, jitter and motion parameters. The recorded configurations remain approximately 16:9 and do not establish an engine-wide arbitrary-aspect mode. |
| **B. Specific shaders enforce 16:9** | No fixed 16:9 enforcement was found in the traced projection, motion or late screen paths. Confirmed aspect-dependent values are the bound camera row ratio, the motion X factor at `ca4beafaad0f3d62+0x210`, and 1920×1080-based coefficients in the UI transform table. A shader patch has no established target here. |
| **C. Static shader evidence is insufficient to decide support** | Supported for the end-to-end question. Missing pieces are the ARM64 projection and mode producers, current/previous and inverse matrix ownership, culling planes and visibility selection, image and dispatch limits, actual per-pass viewport/scissor/render area, final gameplay VI2 source crop, and UI layout ownership. These determine whether the compatible shader interfaces can be driven coherently at 3440×1440. |

## 8. Targeted Ghidra investigation

Use a private local read-only view of the exact network-test ARM64 executable and record its build ID before mapping any functions. All offsets above are buffer or shader offsets. GPU addresses are capture-session values and do not give ARM64 function addresses. Start with the following dataflow anchors.

1. **Camera upload and visibility.** Find the code that populates the buffer reached through graphics slot 4 `+0x20`. Trace the write/copy of the origin at `+0x30` and four consecutive clip rows at `+0x180..0x1bf`. Cross-check against direct graphics slot 3 `+0x80..0xbf` in the effect camera and the camera-motion pass's matrix and scales at slot 3 `+0x00..0x5f`. Look for the perspective construction (`cot(fovY/2)`, aspect division/multiplication, depth terms), jitter injection and render-dimension reciprocals. Follow the unjittered matrix or side-plane construction to the CPU scene visibility/frustum tests and any GPU culling dispatch constants. Identify the previous, current and inverse matrices consumed by reprojection. Search known float values as candidate anchors and confirm with stores and callers.
2. **Dynamic-resolution and reconstruction producers.** Find the writer of the prepass cbuf cluster `+0x198..0x1ec`, keyed by its dimension pair at `+0x1e8/+0x1ec`, and the output cbuf cluster `+0x328/+0x348/+0x358/+0x35c/+0x394/+0x39c`. Look for W/H copies, W−1/H−1, per-axis input/output quotients, and shader/descriptor selection around the two known kernels. Trace which fields feed QMD grid setup, intermediate/history allocation, descriptor extents and block-linear pitch. Find the scene viewport, scissor/surface clip and color/depth/motion target setup using the same active pair. Identify any fixed alignment or maximum width in their producers.
3. **Present allocation and handoff.** Follow the game's present-image registration and submit wrapper, locating the producer of the VI2 `shown_size` word `((H-1)<<16)|(W-1)`. Check its mode lookup and present layout fields independently of the reconstruction extent. Inspect calls corresponding to Lichii's `GetBufferMaxResolution` if this executable uses that query. The repository documents Fire Emblem's mode mapping, with no equivalent Duskbloods mapping established. The existing mode override selects an enum and has no verified 3440×1440 entry for this title. [VI2 resolution query](../src/core/hle/service/nx2_gsv.h#L2382).
4. **UI layout producer.** Follow the late graphics slot-3 64 KiB transform table back to the writer for the reciprocal entries at `+0x80/+0x94` and repeated record at `+0xc0`. Identify the vertex attribute carrying the matrix index, coordinate space of the pane vertices, viewport/scissor selection and whether the UI layout runs before or after scene compositing. Check where menu hit-test or cursor transforms share that layout.

Only if these static anchors fail to identify the producer, obtain one additional targeted observation for the gap. For camera ownership, correlate the already identified shader with its exact cbuf/storage bind and matrix writer. For presentation/UI, capture one gameplay present with the final late draw target, its viewport/scissor and VI2 shown extent. The existing frame/debug and RenderDoc routes can be used for those specific records with an owner-launched capture-enabled build. Reuse the existing evidence before adding instrumentation.

## 9. Small reversible experiment after the producer is identified

Use a local, title- and build-scoped guest override with an off switch, preferably at the verified game mode/camera setup entry points. An emulator hook is justified only for a verified control boundary that cannot be overridden in the guest, such as a final VI2 crop or a specific guest buffer producer. A shader-only change would leave CPU culling, render-target geometry, motion and present data outside its control.

1. Reproduce one stationary 16:9 gameplay shot at the same camera pose. Compare against the wide override at the same vertical framing. Choose docked mode and 1× emulator scaling to keep the guest and host image measurements separable.
2. Set the game-facing output/present rectangle and matching allocations to **3440×1440**, with matching color, depth, motion and history extents for their roles. Hold dynamic resolution at one guest-supported same-aspect scale first. `1720×720` is an example candidate if the guest tiling and backend ranges admit it. Make the viewport/scissor, source bounds, prepass/output constants, QMD dispatch coverage, consumers and present crop agree. Use the existing replacement only if it accepts the coherent contract. Compare with a game-supported 1:1 or native path if it is available and useful for isolating reconstruction.
3. Keep vertical FOV, depth mapping, camera pose and center fixed. For a conventional symmetric projection, the unjittered horizontal scale is `cot(fovY/2)/aspect`. The multiplier relative to 16:9 is `(16/9)/(3440/1440) = 32/43 ≈ 0.744186`. Apply that at the camera producer and rebuild the inverse, frustum, current and previous matrices consistently. Compute jitter in the new active render-pixel grid and reset temporal history on the switch. If the validated scene and overlay paths are separate, retain a centered 2560×1440 UI region for the first test.
4. Compare the wide center 2560×1440 against the 16:9 reference at matched scale. The center should retain vertical framing and object proportions, with genuinely new world geometry in both 440-pixel side strips. Pan across near-edge geometry, occluders and LOD boundaries to detect stale culling. Inspect depth and motion at the center and new edges during pan and disocclusion, then test a static frame for temporal stability and a transition for correct reset. Confirm resource bounds, dispatch coverage, no validation errors or unwritten seams, actual 3440×1440 guest present and host destination, and stable HUD anchors and text proportions. Revert the switch and reproduce the baseline before trying dynamic scale changes.

The report and inventory are the only repository deliverables. All decoding, cache extraction, replay and image comparison used local scratch files. The capture, executable, CMake edits and Lichii runtime code were read-only throughout.

## Appendix. Native program inventory

Each entry gives the full native program key, verified stage and GPU program base address(es). Reconstruct filenames as `<key>-gfx-<address>.bin` or `<key>-cs-<address>.bin`, omitting the address's `0x` prefix. Graphics sidecars use `.header.bin` in place of `.bin`.

<details>
<summary>401 program keys and 455 program addresses</summary>

| Native key | Stage | Program base address(es) |
|---|---|---|
| `001a8c920561a80a` | fragment | `0x2323147c00` |
| `00fe7d8956d5cb68` | fragment | `0x2313cb2e00` |
| `01c75c940b51e3b5` | fragment | `0x231ef8fe00`, `0x231ef90c00` |
| `020ccaf3451f2c01` | fragment | `0x23139fdf00` |
| `039ec4e0a57ccd02` | fragment | `0x2315f1df00` |
| `058385b27081af44` | tess-eval | `0x231335cc00` |
| `061edb352ab3fe15` | fragment | `0x2313b1a200` |
| `06f24823640f85c5` | vertex | `0x2313cff100` |
| `072d19be77984f6f` | fragment | `0x2313a8eb00` |
| `07c47196ae39034c` | vertex | `0x231f162200`, `0x2320e72800`, `0x23214bbc00` |
| `07f96d5a94612c13` | fragment | `0x2316028900`, `0x231dbe0b00`, `0x2325222700` |
| `085fccf11e6f5e9c` | fragment | `0x2320f56f00` |
| `08654048cd7e6e7f` | fragment | `0x2312979a00` |
| `08eda621523da94c` | fragment | `0x232005f800` |
| `092736b1a657b6de` | fragment | `0x2320df2400` |
| `098aa8f90cb4c569` | compute | `0x2312937d00` |
| `0992e330d461646a` | vertex | `0x2313c5cf00` |
| `09bda87e082df0cb` | vertex | `0x231c26d900` |
| `0a445ca9ba316344` | fragment | `0x23129c7f00` |
| `0b015cd6abdfa6d4` | vertex | `0x2320d9d100` |
| `0bdf40f68557fd05` | compute | `0x2312947600` |
| `0c0dcf4db9fd0218` | fragment | `0x231887a200` |
| `0cbf774bc3b02cbd` | compute | `0x21ec6de000` |
| `0e3d0fea3792368d` | vertex | `0x23290fe600` |
| `0e664e3effd98281` | fragment | `0x2323070300` |
| `0e8db3858ee9a1a7` | vertex | `0x2323746800` |
| `0f66fc9971922ba3` | fragment | `0x2313b7ee00` |
| `0f6e14ed5877503b` | vertex | `0x2321907900` |
| `1080a1a364a6f783` | fragment | `0x2313cddb00` |
| `11557bcbef0882c4` | compute | `0x23128d1800` |
| `11b6a465102970b6` | vertex | `0x2320ac3b00` |
| `11bd7b17449f7490` | fragment | `0x231dbb8e00` |
| `125c8187dadb57d0` | fragment | `0x2316028c00` |
| `13212cd2120ced1a` | fragment | `0x2313c71600` |
| `13664391966f217e` | fragment | `0x23212b2300` |
| `138d5a302355b8c7` | vertex | `0x231d5a3500`, `0x231db36800` |
| `14081880aff43431` | fragment | `0x23211b7c00` |
| `143f377ba7ab3689` | compute | `0x2313430600` |
| `1614753ad2986667` | fragment | `0x231c27be00` |
| `163c7e0da8814ccb` | fragment | `0x2321a1fa00` |
| `17542040cd36c4a6` | fragment | `0x2320e77100` |
| `17b9bbfcaf264721` | vertex | `0x2312710f00` |
| `19b7c09131dee277` | vertex | `0x2323571000`, `0x2323775800` |
| `1a751b791f9a2205` | compute | `0x21e9af0000` |
| `1a902e13149f2c3a` | fragment | `0x2318889c00` |
| `1ab85ce56452269c` | fragment | `0x23214be300` |
| `1aef35a7fbc97086` | vertex | `0x2320ad8900` |
| `1bffa220e16a2ca5` | compute | `0x21ec7ad000` |
| `1c26b6fd35ae5376` | fragment | `0x2320df1600` |
| `1c7a80704abdcc32` | vertex | `0x231db73d00` |
| `1e2e6669e243a84b` | vertex | `0x23210c0100` |
| `1f14a22d1346f165` | fragment | `0x2323c98800` |
| `1f6504441b6b15ea` | compute | `0x2312948000` |
| `201286ade6a7821c` | fragment | `0x231ef91b00` |
| `207b94e565848897` | fragment | `0x2313ceb000` |
| `207ec41fe734231e` | fragment | `0x23214bc600` |
| `219e39264a41b41a` | compute | `0x2313c65100` |
| `2247087ef5253036` | vertex | `0x231c2f3900` |
| `231ca4a17594cf81` | fragment | `0x23129fc500` |
| `237f5d25ea909ea3` | vertex | `0x2322300300`, `0x232306fa00` |
| `23ce664ad7ad0028` | fragment | `0x231db1e000` |
| `25d4cc5567521d71` | vertex | `0x2321227f00` |
| `260c5d4bd1d6cf1b` | tess-control | `0x2313267500` |
| `26275ce4cbddb1bc` | fragment | `0x23210c5700` |
| `2672bd30f4cc7bbd` | fragment | `0x232006a700` |
| `27d702ebdda5f20e` | fragment | `0x2313cfb100` |
| `27f400c161179cac` | fragment | `0x232104d500` |
| `282e0e730c40002e` | fragment | `0x23129de500` |
| `285e460e993b6b7d` | fragment | `0x2320bc6100` |
| `288f188cbad5d1ed` | fragment | `0x2313ce1200` |
| `28940bed87c6fdb2` | fragment | `0x2313ab4600` |
| `29417f51cae6edc1` | vertex | `0x2312710300` |
| `298ea2f1928225fd` | fragment | `0x2321045200` |
| `29f7fabd26ffd91d` | vertex | `0x232005ee00` |
| `2abce208c834fd96` | compute | `0x23127eb500` |
| `2b8d7d2779d39d1f` | tess-control | `0x231326ac00` |
| `2bf39ca9781e1627` | fragment | `0x2320f51d00` |
| `2c3290585dce6d1c` | vertex | `0x2312f95000` |
| `2cfc14894b3ab347` | fragment | `0x2320ac7e00` |
| `2d6a8ee32744c2c5` | compute | `0x23128f5600` |
| `2d88dce1152b6eb0` | fragment | `0x2313ada400` |
| `2e4908dc0a801b05` | fragment | `0x231d99e900` |
| `2ece14d52e9d4bc2` | fragment | `0x23222dec00` |
| `2eef70520b9e62e3` | fragment | `0x2323ccc400` |
| `2f3028dddfd3d307` | compute | `0x21ec7d2000` |
| `2f4e69ab84eef2df` | fragment | `0x2313cdbd00` |
| `31c39f71a86e7859` | fragment | `0x231c2b5900` |
| `32dc15975348966b` | vertex | `0x2313c5c300` |
| `338ca307f5c70f21` | vertex | `0x231f9c5800`, `0x2321727100` |
| `3452963b939ceab9` | vertex | `0x2320069600` |
| `3751d103935c3795` | fragment | `0x23129d8100` |
| `37cd36b0ac64f3af` | fragment | `0x2313cb8d00` |
| `39040c00cc1ccf59` | vertex | `0x2312f9b900` |
| `3a0c4a05163cc9bd` | fragment | `0x2313ce0f00` |
| `3b4540c00f2cb355` | vertex | `0x2323136900` |
| `3c0a459fbf9d1ee9` | fragment | `0x2313af6d00` |
| `3c364069e3033b6c` | fragment | `0x23129ba200` |
| `3c56fc689716d091` | fragment | `0x2321032d00` |
| `3c7cb4e50f597949` | compute | `0x23128caf00` |
| `3e58ab1283d2b75c` | vertex | `0x231c26c000` |
| `3e740193be74aa06` | fragment | `0x2320adc700` |
| `3f14cebc1a4bac71` | fragment | `0x2320e83100`, `0x23214c8e00`, `0x23214c9500` |
| `3f7b00cfae6b3bbf` | fragment | `0x2321704900` |
| `3fdce16079cb5e00` | fragment | `0x2313cf5d00` |
| `419997276a9c9d69` | fragment | `0x231dbe0e00` |
| `42f8287eb88c301e` | fragment | `0x2320df6f00` |
| `43a35bada86d20be` | fragment | `0x23129d2900` |
| `43df263cf7842da0` | fragment | `0x2321558700` |
| `4649c75c8fa86189` | fragment | `0x2313ce4700` |
| `47908e86f09f8212` | fragment | `0x2313ccb900` |
| `47cf15fb7bae56c1` | vertex | `0x231c1f3d00` |
| `499ac83ae02124c8` | fragment | `0x231c28f600` |
| `4a40468aa41aa23d` | fragment | `0x2313ce8b00` |
| `4aa2746bf38dc79a` | fragment | `0x23290fd700` |
| `4ae9e610809a131b` | tess-eval | `0x231334e800` |
| `4b02de9c503a9d18` | compute | `0x21ec705000` |
| `4bdad791cb26baf8` | fragment | `0x231297b200` |
| `4c2a80ebb71bbd68` | fragment | `0x231fde5300` |
| `4d08ae0bf160f4b7` | fragment | `0x23129fe100` |
| `4d7a348539309501` | tess-eval | `0x2313352a00` |
| `4f661db6a9badc8f` | fragment | `0x2313cb4200` |
| `4f872257f3e925cf` | fragment | `0x23218fbb00` |
| `50e47e31c31fc1f8` | compute | `0x21ec8ae000` |
| `515e8624d29dfcb3` | fragment | `0x23129d3300` |
| `52679ba2e5a1de57` | fragment | `0x23129d8d00` |
| `5306a4551a7022be` | tess-eval | `0x231339df00` |
| `5382d8b09927eb02` | fragment | `0x231ef6de00` |
| `54405c07ed8e8dfb` | vertex | `0x23222de300` |
| `548538716d870d81` | fragment | `0x2322855100` |
| `54bd1fe8c25cc102` | fragment | `0x231c20d000` |
| `555b500cee52f71e` | fragment | `0x2313ca3c00` |
| `570f5a76e1e06ba5` | vertex | `0x2320df6600` |
| `5858fba2c75e53bd` | fragment | `0x2313c9cb00` |
| `586df1d91b323a82` | fragment | `0x231885e300` |
| `58aed51d1ddbd572` | fragment | `0x2320d9ea00` |
| `58f7c6bd10874ff1` | fragment | `0x2313c92300` |
| `595a573eeacf8206` | fragment | `0x23214c0400` |
| `5a9883279a0a2653` | vertex | `0x23134ffc00`, `0x2313cffc00` |
| `5b3d77cc0e9bb2a8` | fragment | `0x2322f65b00` |
| `5d0c713823c61ec1` | fragment | `0x23139f1b00` |
| `5e6f1bdc72d2e387` | vertex | `0x2313c5cd00` |
| `5eda2b5531ebfc6b` | compute | `0x2312701400` |
| `60c96eddc75b74f9` | fragment | `0x231d5bf500` |
| `61335a915ae95a1b` | fragment | `0x2320063f00` |
| `6133c03db4a7921c` | vertex | `0x2321048d00` |
| `613c0e18c140be65` | fragment | `0x2320062b00` |
| `6184d2332a47a15b` | fragment | `0x2313cf1e00` |
| `62554525ff9b367d` | fragment | `0x231c29de00` |
| `62d5a27668dc1ba2` | fragment | `0x2313cdae00` |
| `63a2df085fb204f6` | fragment | `0x2313b47600` |
| `64ef72653b21ca74` | fragment | `0x2322891000` |
| `6800759e162d47c6` | fragment | `0x2325201600` |
| `68960ee29903f79d` | compute | `0x2312935c00` |
| `68a84477c245d545` | compute | `0x23128cef00` |
| `68ebcfb1b56663aa` | fragment | `0x2321909f00` |
| `69ae412656a4b514` | fragment | `0x2313cda500` |
| `6a31d7b8d5757b6f` | fragment | `0x2313cf6300` |
| `6a4159714d496e09` | fragment | `0x23129d7e00` |
| `6b40c2eaa5310680` | fragment | `0x23139c8200` |
| `6bae406a3dd259be` | fragment | `0x2313cda700` |
| `6bba6d544aeb6d13` | fragment | `0x2323741200` |
| `6c4efd016ae7fe12` | fragment | `0x2313c93600` |
| `6c515b3fcdda35d8` | fragment | `0x2313b77d00` |
| `6c69a9054c24d1ca` | fragment | `0x2313ca6e00` |
| `6d621d9caa85d3a5` | fragment | `0x2313c91100` |
| `6e418b2ecbc94308` | fragment | `0x2313b40e00` |
| `6f06f27a0de66df5` | compute | `0x2313c62600` |
| `6f19de38fd83800d` | fragment | `0x2313caec00` |
| `6f1b6d537c8624a4` | fragment | `0x231c296400` |
| `6f44ad5330b5bc95` | fragment | `0x231c204600` |
| `703f87978a85c036` | fragment | `0x2313cc8100` |
| `70675447b8b81e70` | compute | `0x2312943500` |
| `70684714f1c83198` | fragment | `0x2313c78b00` |
| `70f1d0bc9dceaca1` | compute | `0x21ec6b3000` |
| `70f8a0a7ac25bb9a` | fragment | `0x23129ec100` |
| `72118d6d849ba72e` | vertex | `0x231ef8d100` |
| `7228ab2a96ab75c4` | vertex | `0x2312713a00`, `0x2313c5cb00` |
| `7266977f1d72d23c` | vertex | `0x2320ad3100` |
| `7303932a5b040331` | fragment | `0x2313c72e00` |
| `7334f3971e66230d` | fragment | `0x23218b1d00` |
| `73d3e20a8f39c476` | compute | `0x21e7a20000` |
| `7401744e4fb20b7e` | fragment | `0x23290fe900` |
| `7434e326497c0085` | vertex | `0x2321a26000` |
| `7471be4966f1712b` | compute | `0x21ea137000` |
| `75ade40053fecef3` | fragment | `0x2313c59500` |
| `7623f939c701e462` | vertex | `0x232150f700` |
| `770a28b67f716fb8` | vertex | `0x2322207900` |
| `77122e4d28fc937e` | fragment | `0x2313a68400` |
| `77d5cde6ee2a0a13` | vertex | `0x2312702700` |
| `78eef89a9ad6ace2` | fragment | `0x23134f7100`, `0x2313cf7100` |
| `7a261a9132bb3166` | vertex | `0x2311f01b00`, `0x2312701b00` |
| `7a438c96b19d3bd4` | compute | `0x2313c5ea00` |
| `7b77e185ec5ea599` | fragment | `0x2320bd2900` |
| `7b7f9bf63cd7e871` | compute | `0x2313c6a600` |
| `7b9a84bdb9407295` | vertex | `0x231f9bd300`, `0x232160a800`, `0x23218b0d00` |
| `7d6f62721f528d97` | fragment | `0x231d951500` |
| `7eabd2ae5cf37ad1` | vertex | `0x231dbdf700` |
| `7f77dd85e565ea8d` | compute | `0x2313c67600` |
| `7f7c7e8fb7ac16e5` | fragment | `0x2320ad6600` |
| `7f8a99ca1334f337` | fragment | `0x231d52b100` |
| `7fd7d13f87726b90` | fragment | `0x2313cb7e00` |
| `806f1d07d4359be4` | fragment | `0x2313cd4b00` |
| `8174b137663d3eea` | vertex | `0x23134fec00`, `0x2313cfec00` |
| `825cb516385a9f2c` | compute | `0x23128ccd00` |
| `82bcecf51b40b341` | fragment | `0x2313cb1400` |
| `82e3702baaddb97f` | fragment | `0x232122ad00` |
| `833414760885c606` | fragment | `0x23129b0f00` |
| `83521809ee86ae3d` | fragment | `0x23210d5c00` |
| `83ab38c1a33d890f` | tess-eval | `0x23133bfe00` |
| `844df235684d5acd` | vertex | `0x2312f8c800` |
| `861cbdf5974d6c21` | fragment | `0x2320e78200` |
| `8642879cd7831386` | fragment | `0x2313c70600` |
| `87292d9d0307cc0c` | vertex | `0x2312a72700` |
| `8884f59bd9d6bc90` | fragment | `0x2312a00a00` |
| `88b98ca58ace17b8` | fragment | `0x23134f5100`, `0x2313cf5100` |
| `8a5e4aeca93d1a37` | vertex | `0x232019b300`, `0x23212b8200` |
| `8a981917d1a496b8` | fragment | `0x231f162c00` |
| `8af447db079e4a40` | fragment | `0x2321225f00` |
| `8b357e2ab2ad7130` | vertex | `0x2312f98300` |
| `8c704d3bfc6a1e72` | fragment | `0x2320195300` |
| `8d1500b0ad945064` | vertex | `0x2312f97b00` |
| `8d2d88776d9738b5` | fragment | `0x2312a03800` |
| `8d920346bcb85349` | tess-eval | `0x23133d2400` |
| `8e4a90e0cae217e7` | fragment | `0x2323175400` |
| `8e7d573b4991a418` | vertex | `0x2320e80100`, `0x23214c8400` |
| `8ec4a46551a683b5` | compute | `0x2312757f00` |
| `8efc842e7ef29f56` | vertex | `0x23211ce300` |
| `8f119e6f3610f754` | vertex | `0x2312a71700` |
| `8f81e3c344823155` | fragment | `0x231c20b800` |
| `8fdd2a0854efbad6` | vertex | `0x231ef66300` |
| `8ff0a3cc0b3e1916` | fragment | `0x231ef6a500` |
| `911263e57f097a5c` | tess-eval | `0x23133bc500` |
| `91cad8fea383f39b` | fragment | `0x231f81ec00` |
| `9202e0f58aeeea41` | compute | `0x231293bb00` |
| `9372120f39e2b7ad` | fragment | `0x231f164900` |
| `9381fa3b7b3dc6e3` | fragment | `0x231c279200`, `0x231c279d00`, `0x231d5a8200`, `0x231db3cb00` |
| `9525dd2b198553f3` | fragment | `0x2313a42000` |
| `96542c50e7417ce1` | fragment | `0x2312a02d00` |
| `97d6622d25f3c819` | fragment | `0x2323d0c500` |
| `9872f22102e467db` | vertex | `0x2323cd5e00` |
| `9a603744f882a89d` | fragment | `0x2315f28200` |
| `9b47b95e0fb51205` | vertex | `0x23211b3c00` |
| `9b930a7e944b96ed` | compute | `0x2312937900` |
| `9c173ec1730447f6` | compute | `0x23128caa00` |
| `9d5415a1ffe50308` | vertex | `0x2321223c00` |
| `9d9050fd24463131` | tess-eval | `0x2313392100` |
| `9dd698fce3d5bf29` | tess-control | `0x2313265b00` |
| `9ee87024ef12b582` | vertex | `0x2312713800` |
| `9f3155cc66b7cbe1` | vertex | `0x2320df0d00` |
| `9f35f563b9410ff2` | vertex | `0x2313c32300`, `0x2313c32700` |
| `9f56b9c887247b7f` | compute | `0x21ec68a000` |
| `a0997f5142c397fb` | compute | `0x21ec726000` |
| `a0fb3921f64484b6` | tess-control | `0x2313266400` |
| `a19b1dcfa9ea8da7` | tess-eval | `0x2313364e00`, `0x231337a400` |
| `a2031baf4b3d9fb6` | fragment | `0x231ef68900`, `0x231ef6fb00` |
| `a4533325eb6ab7fb` | vertex | `0x23218f7b00`, `0x2322f65200`, `0x23251e2700`, `0x2325a64a00` |
| `a4fad2290fc28678` | fragment | `0x2322379c00` |
| `a51007b4f9ebe6dd` | tess-control | `0x2313267000` |
| `a5112eddeedae9eb` | compute | `0x2312759a00` |
| `a518d65e60a8154a` | fragment | `0x2321a26900`, `0x2323747600` |
| `a558181d6ef73d7e` | vertex | `0x2312f99d00` |
| `a57226692480120d` | fragment | `0x23290fcf00` |
| `a88afd163325e94e` | fragment | `0x2313a8c900` |
| `a8a18c0c2be85689` | fragment | `0x2311f05600`, `0x2312705600` |
| `a8b146fbe1c81109` | fragment | `0x2313cdeb00` |
| `a8f5cf388503ca6a` | fragment | `0x232160bd00` |
| `a93612bdc5572943` | fragment | `0x2320196100` |
| `a9847937fd5780c0` | fragment | `0x231d993300` |
| `a99124f3b5a5ef5c` | vertex | `0x2312f96600` |
| `aa7aaeafcaccce92` | tess-control | `0x2313265700` |
| `aa9f073d0a5f85dd` | vertex | `0x2315e68200`, `0x2318855e00`, `0x231c1f6000` |
| `ab471af43f6440a2` | vertex | `0x232102f100` |
| `ab5cc7ff935fd62b` | vertex | `0x2313c5d100`, `0x2313c5d600` |
| `abe99dcd05269207` | fragment | `0x231ef72300` |
| `ac841ba7786fd692` | vertex | `0x2323cc5600`, `0x2323d04b00` |
| `ac87a3a41e4ffc33` | vertex | `0x2320bd1400`, `0x2320f56600` |
| `ac91ee647b947b45` | vertex | `0x2321217600` |
| `ad0fb563e4dc28cb` | vertex | `0x231d5b4a00`, `0x231db41a00` |
| `ad3ac744dddb90ab` | fragment | `0x2313cf1100` |
| `ae1504bb159dc949` | vertex | `0x2312710800` |
| `ae2061d5b4a39d7c` | vertex | `0x2312f9b100` |
| `ae9eb0ac405717fb` | vertex | `0x231d973800` |
| `aec3996162c0265f` | compute | `0x2312718900` |
| `aee3db8a99842f05` | compute | `0x21e99bf000` |
| `af44dfa1050c02c9` | fragment | `0x2313ce7100` |
| `b0d661531a61117f` | vertex | `0x2316021d00`, `0x231887f000`, `0x231c284f00` |
| `b3ac4ea4170899ce` | fragment | `0x2313a6a400` |
| `b49cbb7241e6c3bb` | fragment | `0x2313c49400` |
| `b5686fa5e39e52e7` | fragment | `0x232019c500` |
| `b70394e7a665f103` | vertex | `0x2312702400` |
| `b7a5d627e74b6416` | fragment | `0x23134f8500`, `0x2313cf8500` |
| `b9bf02095c2186b8` | fragment | `0x2321a21200` |
| `b9ece8a9f0b7e4df` | fragment | `0x2313cd7200` |
| `b9f6475f2bca1d69` | fragment | `0x2321729700` |
| `ba743b8220afe3fc` | vertex | `0x232521f300` |
| `bb0ccb51a19783a9` | vertex | `0x231d985f00` |
| `bba6591979674b83` | fragment | `0x2313cdb100` |
| `bc129bbb5c0ddfdd` | fragment | `0x2313c4ca00` |
| `bc2726e4774e497f` | vertex | `0x231d521a00`, `0x231db05600` |
| `bc37dca25b7654d3` | compute | `0x2312700200` |
| `bd13bcd95884ca34` | fragment | `0x231c221700` |
| `be23c604bd7b4446` | fragment | `0x23129d2c00` |
| `bf1a976af3133592` | vertex | `0x231c2f5a00`, `0x231db68500` |
| `bfa8a3037041a7f6` | vertex | `0x23290fe300` |
| `c12e0e84d4cbb5d7` | fragment | `0x23290fa500` |
| `c205885b7ffb0526` | tess-eval | `0x23133b0a00` |
| `c25be5108bf26e6e` | vertex | `0x2320192600`, `0x23212b1900` |
| `c2e451acb7f9306a` | fragment | `0x2320d9db00` |
| `c2f3ec47b8d6c97f` | fragment | `0x2312bdd000` |
| `c42a1a6f59e62ed7` | fragment | `0x232160b100` |
| `c58e8fe2055d266e` | compute | `0x21e9aab000` |
| `c5a89d440586a693` | compute | `0x2313c64100` |
| `c64b6f32978a9f3f` | tess-control | `0x231326b000` |
| `c70bc2e6d2082fdd` | fragment | `0x23129d5000` |
| `c78644e5c43d2eea` | tess-eval | `0x2313343100` |
| `c818e725633c9aa9` | fragment | `0x231ef6b000` |
| `c99130e5e7433b24` | vertex | `0x2321a1f100` |
| `c9a7a3d040516c78` | vertex | `0x23210cd200` |
| `c9e6784cf53404f8` | fragment | `0x23134fb500`, `0x2313cfb500` |
| `ca4beafaad0f3d62` | fragment | `0x231294a100` |
| `cba04903f1e74e23` | compute | `0x2312940e00` |
| `cc9a65ce387e426e` | fragment | `0x23222e0700` |
| `cd3116e824fd2108` | fragment | `0x23139ffd00` |
| `ce3f45e721dd4c3d` | fragment | `0x2323776600` |
| `ce8c27fec52fe064` | fragment | `0x2322330a00` |
| `cf68b5db5dc6d4fc` | fragment | `0x2313c9bb00` |
| `d0d79495e9d522c7` | fragment | `0x231db5be00` |
| `d13a041b01c397db` | tess-eval | `0x2313368500` |
| `d1a16210d336c33d` | fragment | `0x2313b44f00` |
| `d1a2a9932422b321` | fragment | `0x2313ab2800` |
| `d2722424198c0195` | fragment | `0x2312707400` |
| `d2aef28f4fd357f7` | fragment | `0x23212b3800` |
| `d475950e3f1c123f` | fragment | `0x231295c200` |
| `d50f78ed479f3e8d` | fragment | `0x23216fc000` |
| `d5875746b5ba74aa` | vertex | `0x2313cff500` |
| `d5bd72177c1d97e5` | fragment | `0x23212b8c00` |
| `d6e493329909c857` | fragment | `0x23216b8200` |
| `d74534a5887a989c` | fragment | `0x231dbaef00` |
| `d74b0fd6133f917d` | fragment | `0x2313a93d00` |
| `d78b2268268e9a4b` | fragment | `0x23211ca700` |
| `d8134f1ffe021768` | fragment | `0x2313cade00` |
| `d8ace31b95183ed1` | vertex | `0x231d93bc00` |
| `d94ad172c48ce19a` | fragment | `0x23129b8100` |
| `d9dc9aea1f43f8bb` | tess-eval | `0x2313356100`, `0x2313373900` |
| `da268b3a8b740716` | fragment | `0x2313b43a00` |
| `da7d798f1a3704d0` | fragment | `0x2311f0b100`, `0x231270b100` |
| `dac4f90ba35f2469` | fragment | `0x2323cdaa00` |
| `db650aba76daa995` | vertex | `0x2318875900`, `0x231c273e00` |
| `dcc9b6b8a44eb8c1` | vertex | `0x231d97a300` |
| `df16a1df76d77383` | vertex | `0x2313c5d300` |
| `e06409c93b62d06c` | tess-control | `0x231326b800` |
| `e0db459dd1a8a988` | compute | `0x21ec750000` |
| `e0f3ccb5aa72f80b` | fragment | `0x231feb0400` |
| `e11f46a1f1289142` | vertex | `0x2313c5c900` |
| `e1951351919abb4d` | fragment | `0x231f9c0900` |
| `e1b3702b80f7e1b7` | vertex | `0x232103e400` |
| `e1c505dd6dc9d27d` | vertex | `0x231c271b00` |
| `e1ed9e7b44f46d79` | vertex | `0x231feabd00` |
| `e32d1d10ff10857a` | fragment | `0x2312708200` |
| `e44cc77b33770c6b` | fragment | `0x23211d2e00` |
| `e51f94e2ea0b6223` | fragment | `0x231d97f500` |
| `e52e9474c86e3c2d` | fragment | `0x2320d9fe00` |
| `e53e42e7f06ceb12` | fragment | `0x231feaf200` |
| `e54560885337000d` | fragment | `0x2313b1d100` |
| `e5efe04581720f15` | compute | `0x21e9e53000` |
| `e64328e728a39a64` | fragment | `0x2320f50d00` |
| `e6854749f0b120d9` | fragment | `0x231fde4200` |
| `e6e8dc1726e96e43` | vertex | `0x2312f95b00` |
| `e73409766b5a4453` | fragment | `0x231d946800` |
| `e73a6d66ce8df831` | fragment | `0x2313c74500` |
| `e7daa174df6eb839` | fragment | `0x231f9c6900` |
| `e80ab10b7c53e6ca` | vertex | `0x231c281c00` |
| `eb31e3e1cfd18401` | fragment | `0x2313cb0400` |
| `ebfb8a451708bf15` | fragment | `0x2321510000` |
| `ee3809cb37f22066` | compute | `0x21ec783000` |
| `ee5f1352bcb35b53` | fragment | `0x23129a1500` |
| `ef9591651b2e5b36` | fragment | `0x2313a47800` |
| `f031c8e8eb416e8f` | vertex | `0x231f81b700`, `0x2320bc2e00`, `0x2320f50400`, `0x2321557e00`, `0x23216e9c00`, `0x232284f000`, `0x2322872100` |
| `f258bf174f982884` | fragment | `0x2313ca3700` |
| `f26552c876e54667` | vertex | `0x23216b7800` |
| `f268942ab500bab7` | vertex | `0x2323c36e00` |
| `f2e9a1ae9f73e589` | fragment | `0x232121a500` |
| `f36b6e5c2ed3b200` | fragment | `0x232377d600` |
| `f3d084df7f390891` | vertex | `0x231fddf300` |
| `f4577b946d0ef559` | vertex | `0x23211c7500` |
| `f4b48cdebae66bed` | fragment | `0x2322208300` |
| `f5ce151960003d29` | fragment | `0x2312707000` |
| `f6381cd92302d1d8` | fragment | `0x2325a80600` |
| `f6daf3102dbcad57` | fragment | `0x232362d400` |
| `f8c4182d364f92e8` | tess-eval | `0x23133c1d00` |
| `fa5affa5abb1fc34` | fragment | `0x2313cb1800` |
| `fb039df1a0376339` | fragment | `0x23251ee000` |
| `fb0da55fd6055db2` | vertex | `0x2321042200` |
| `fb350b0b9e13fad9` | fragment | `0x231ef6ea00` |
| `fb65c8f3da7678a8` | compute | `0x2313c60000` |
| `fc59e5443c437edd` | fragment | `0x23290fd100` |
| `fd527b9ad5d3e5a5` | fragment | `0x2313c70a00` |
| `fd90470501ac8398` | fragment | `0x2322f67b00` |
| `fda97a3c3a8c9dfc` | vertex | `0x2312713400` |
| `fdb515737945678e` | vertex | `0x232373fe00` |
| `fe9c6227152ffb7c` | fragment | `0x231ef70700` |

</details>
