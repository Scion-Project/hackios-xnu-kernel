// SPDX-License-Identifier: MIT
// The library the bridge's self-test runs (nmb_selftest, tools/metal/bridge/NativeMetalBridge.rs), compiled to AIR bitcode by
// tools/metal/mk-metal2vulkan.sh with Xcode's Metal compiler and embedded in libmetal2vulkan.dylib. The three functions are the ones the
// self-test asks for by name, with the meaning it checks: 1000 floats i become 2*i + i = 3*i; an 8x8 target cleared blue gets its
// top-left quarter (two triangles) in (1, 0.2, 0, 1); and the same quad over 64x64 imported host pages. Written for Scion; the
// self-test itself is AdrenoMetal's.
#include <metal_stdlib>
using namespace metal;

kernel void scale_and_offset(device const float *input [[buffer(0)]],
                             device float *output [[buffer(1)]],
                             constant float &scale [[buffer(2)]],
                             uint i [[thread_position_in_grid]])
{
	output[i] = input[i] * scale + float(i);
}

struct Corner {
	float4 position [[position]];
	float4 color [[flat]];
};

vertex Corner colored_vertex(device const float2 *positions [[buffer(0)]],
                             device const float4 *colors [[buffer(1)]],
                             uint v [[vertex_id]])
{
	Corner out;
	out.position = float4(positions[v], 0.0, 1.0);
	out.color = colors[v];
	return out;
}

fragment float4 colored_fragment(Corner in [[stage_in]])
{
	return in.color;
}
