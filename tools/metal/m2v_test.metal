// SPDX-License-Identifier: MIT
// The Metal shaders of tools/metal/m2v_test.c: compiled by Xcode's Metal compiler into a metallib (tools/metal/mk-metal2vulkan.sh mac)
// and run through metal2vulkan on a Vulkan driver of the Mac; every result is checked against the same arithmetic done on the CPU.
#include <metal_stdlib>
using namespace metal;

// out = a * k + b over whole numbers that a float holds exactly, so the CPU's answer is the only right one
kernel void axpb(device const float *a [[buffer(0)]],
                 device const float *b [[buffer(1)]],
                 device float *out [[buffer(2)]],
                 constant float &k [[buffer(3)]],
                 uint i [[thread_position_in_grid]])
{
	out[i] = a[i] * k + b[i];
}

// loops and branches: the number of Collatz steps from n = i + 1 down to 1 (capped at 1000)
kernel void collatz(device uint *steps [[buffer(0)]],
                    uint i [[thread_position_in_grid]])
{
	uint n = i + 1, s = 0;
	while (n != 1 && s < 1000) {
		n = (n & 1) ? 3 * n + 1 : n >> 1;
		s++;
	}
	steps[i] = s;
}

// threadgroup memory and barriers: each group of 256 sums its values in a tree, and one thread of each adds the group's sum to the
// total with a device atomic
kernel void group_sum(device const uint *values [[buffer(0)]],
                      device atomic_uint *total [[buffer(1)]],
                      device uint *partial [[buffer(2)]],
                      uint i [[thread_position_in_grid]],
                      uint t [[thread_position_in_threadgroup]],
                      uint g [[threadgroup_position_in_grid]])
{
	threadgroup uint scratch[256];
	scratch[t] = values[i];
	threadgroup_barrier(mem_flags::mem_threadgroup);
	for (uint half_ = 128; half_ > 0; half_ >>= 1) {
		if (t < half_) {
			scratch[t] += scratch[t + half_];
		}
		threadgroup_barrier(mem_flags::mem_threadgroup);
	}
	if (t == 0) {
		partial[g] = scratch[0];
		atomic_fetch_add_explicit(total, scratch[0], memory_order_relaxed);
	}
}

// the triangle: three corners from the vertex number, red, green and blue, the colour interpolated between them
struct Shaded {
	float4 position [[position]];
	float4 color;
};

vertex Shaded triangle_vertex(constant float2 *corners [[buffer(0)]],
                              uint v [[vertex_id]])
{
	const float4 colors[3] = { float4(1, 0, 0, 1), float4(0, 1, 0, 1), float4(0, 0, 1, 1) };
	Shaded out;
	out.position = float4(corners[v], 0.0, 1.0);
	out.color = colors[v];
	return out;
}

fragment float4 triangle_fragment(Shaded in [[stage_in]])
{
	return in.color;
}
