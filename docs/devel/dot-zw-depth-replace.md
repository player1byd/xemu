# Texture shader depth replacement

`PS_TEXTUREMODES_DOT_ZW` replaces fragment window depth with the preceding
`DOTPRODUCT` result divided by the current dot product. It replaces interpolated
depth and polygon offset. The Xbox texture coordinates express guest window-depth
units; the regular D16/D24 conversion still applies. An inconsistent stage acts
as `NONE` when its preceding dot or selected previous texture result is unusable.

Reference: [NVIDIA texture shader specification, section 3.8.13.1.21](https://registry.khronos.org/OpenGL/extensions/NV/NV_texture_shader.txt).

The generated-shader regression executes the production GLSL on OpenGL. Run
`test-xbox-psh-depth-replace` with an OpenGL 4 context (for example under Xvfb
with Mesa). Its TAP output explicitly skips when no context is available unless
`XEMU_REQUIRE_GL_TEST_CONTEXT=1` is set; required CI uses that flag under Xvfb.
`test-xbox-vk-ubershader-glsl-integration` also compiles both valid stages and
integer depth formats through the real Vulkan compiler, including the uber route.
Compiler coverage does not establish Vulkan rendering parity.

## Suggested hardware parity XBE

A pbkit test should draw independently labeled tiles into D16 and D24S8 targets:

1. Clear depth to its format maximum. Submit a triangle/quad with geometric depth
   zero, `PASSTHRU` stage 0, `DOTPRODUCT` stage 1 and `DOT_ZW` stage 2. Set stage 0
   to `(1,0,0,1)`, numerator coordinates to `(0.75 * maximum,0,0)`, and denominator
   coordinates to `(1,0,0)`. Read back depth; expect 0.75 of the maximum, subject
   to integer conversion.
2. Repeat with `DOTPRODUCT` stage 2 and `DOT_ZW` stage 3, reading stage 0 for both
   dot products. Draw blue geometry at half depth afterward. It must remain visible.
3. Repeat with geometric depth outside the clip range and a large polygon offset.
   Neither should replace the valid texture-computed depth.
4. With depth clipping enabled, try replacement depths below/above the clip range.
   Rejected fragments must leave both color and depth unchanged. Separately test
   disabled depth clipping, reversed ranges, zero denominators and non-finite
   results to establish those hardware edge cases.
5. Use HILO mapping with authored RGBA bytes `(0,0,0,64)`, numerator coordinates
   `(1,1/65535,0)` and denominator `(0,0,1/maximum)`. Expect normalized depth
   approximately 0.250003815. Repeat with all four bytes 255 to exercise clipping.
6. Repeat without `DOT_ZW` and with an inconsistent predecessor. Compare ordinary
   interpolation and inactive-stage behavior with the hardware output.

Run the same XBE on an original Xbox and both xemu backends, retaining color and
depth readbacks and exact register state. This is a proposed parity fixture;
the host regressions do not claim that it has been run on hardware.
