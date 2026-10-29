// SPDX-License-Identifier: MIT
// A kernel with a function constant, embedded in libmetal2vulkan.dylib as nmb_function_constant_air() (the bridge's ABI keeps that
// entry for its checks of newFunctionWithName:constantValues:error:). Compiled by tools/metal/mk-metal2vulkan.sh.
#include <metal_stdlib>
using namespace metal;

constant bool add_index [[function_constant(0)]];
constant float offset [[function_constant(1)]];

kernel void gated_add(device float *data [[buffer(0)]],
                      uint i [[thread_position_in_grid]])
{
	float value = data[i] + offset;
	if (add_index) {
		value += float(i);
	}
	data[i] = value;
}
