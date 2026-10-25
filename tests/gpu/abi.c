/* abi.c - src/wgpu89.h against the webgpu.h it was written from (tests/gpu/webgpu.h, wgpu-native v29): every struct's
 * size and every field's offset, and every constant. ./test builds it as C11 and runs it; it prints what differs. */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "webgpu.h"
typedef uint32_t u32;
typedef uint64_t u64;
#include "../../src/wgpu89.h"

static int bad;
#define SAME(a, b) do { if ((a) != (b)) { printf("differs: %s is %ld, %s is %ld\n", #a, (long)(a), #b, (long)(b)); bad = 1; } } while (0)
#define SIZE(g, w) SAME(sizeof(g), sizeof(w))
#define FIELD(g, w, f) SAME(offsetof(g, f), offsetof(w, f))

int main(void)
{
	SIZE(WgStringView, WGPUStringView); FIELD(WgStringView, WGPUStringView, data); FIELD(WgStringView, WGPUStringView, length);
	SIZE(WgChained, WGPUChainedStruct); FIELD(WgChained, WGPUChainedStruct, next); FIELD(WgChained, WGPUChainedStruct, sType);
	SIZE(WgFuture, WGPUFuture);
	SIZE(WgRequestCallbackInfo, WGPURequestAdapterCallbackInfo);
	FIELD(WgRequestCallbackInfo, WGPURequestAdapterCallbackInfo, mode); FIELD(WgRequestCallbackInfo, WGPURequestAdapterCallbackInfo, callback);
	FIELD(WgRequestCallbackInfo, WGPURequestAdapterCallbackInfo, userdata1); FIELD(WgRequestCallbackInfo, WGPURequestAdapterCallbackInfo, userdata2);
	SIZE(WgRequestCallbackInfo, WGPURequestDeviceCallbackInfo);
	FIELD(WgRequestCallbackInfo, WGPURequestDeviceCallbackInfo, mode); FIELD(WgRequestCallbackInfo, WGPURequestDeviceCallbackInfo, callback);
	FIELD(WgRequestCallbackInfo, WGPURequestDeviceCallbackInfo, userdata1); FIELD(WgRequestCallbackInfo, WGPURequestDeviceCallbackInfo, userdata2);
	SIZE(WgBufferMapCallbackInfo, WGPUBufferMapCallbackInfo);
	FIELD(WgBufferMapCallbackInfo, WGPUBufferMapCallbackInfo, mode); FIELD(WgBufferMapCallbackInfo, WGPUBufferMapCallbackInfo, callback);
	FIELD(WgBufferMapCallbackInfo, WGPUBufferMapCallbackInfo, userdata1); FIELD(WgBufferMapCallbackInfo, WGPUBufferMapCallbackInfo, userdata2);
	SIZE(WgUncapturedErrorCallbackInfo, WGPUUncapturedErrorCallbackInfo);
	FIELD(WgUncapturedErrorCallbackInfo, WGPUUncapturedErrorCallbackInfo, callback);
	FIELD(WgUncapturedErrorCallbackInfo, WGPUUncapturedErrorCallbackInfo, userdata1);
	FIELD(WgUncapturedErrorCallbackInfo, WGPUUncapturedErrorCallbackInfo, userdata2);
	SIZE(WgDeviceLostCallbackInfo, WGPUDeviceLostCallbackInfo);
	FIELD(WgDeviceLostCallbackInfo, WGPUDeviceLostCallbackInfo, mode); FIELD(WgDeviceLostCallbackInfo, WGPUDeviceLostCallbackInfo, callback);
	SIZE(WgRequestAdapterOptions, WGPURequestAdapterOptions);
	FIELD(WgRequestAdapterOptions, WGPURequestAdapterOptions, featureLevel); FIELD(WgRequestAdapterOptions, WGPURequestAdapterOptions, powerPreference);
	FIELD(WgRequestAdapterOptions, WGPURequestAdapterOptions, forceFallbackAdapter); FIELD(WgRequestAdapterOptions, WGPURequestAdapterOptions, backendType);
	FIELD(WgRequestAdapterOptions, WGPURequestAdapterOptions, compatibleSurface);
	SIZE(WgQueueDescriptor, WGPUQueueDescriptor); FIELD(WgQueueDescriptor, WGPUQueueDescriptor, label);
	SIZE(WgDeviceDescriptor, WGPUDeviceDescriptor);
	FIELD(WgDeviceDescriptor, WGPUDeviceDescriptor, label); FIELD(WgDeviceDescriptor, WGPUDeviceDescriptor, requiredFeatureCount);
	FIELD(WgDeviceDescriptor, WGPUDeviceDescriptor, requiredFeatures); FIELD(WgDeviceDescriptor, WGPUDeviceDescriptor, requiredLimits);
	FIELD(WgDeviceDescriptor, WGPUDeviceDescriptor, defaultQueue); FIELD(WgDeviceDescriptor, WGPUDeviceDescriptor, deviceLostCallbackInfo);
	FIELD(WgDeviceDescriptor, WGPUDeviceDescriptor, uncapturedErrorCallbackInfo);
	SIZE(WgBufferDescriptor, WGPUBufferDescriptor);
	FIELD(WgBufferDescriptor, WGPUBufferDescriptor, label); FIELD(WgBufferDescriptor, WGPUBufferDescriptor, usage);
	FIELD(WgBufferDescriptor, WGPUBufferDescriptor, size); FIELD(WgBufferDescriptor, WGPUBufferDescriptor, mappedAtCreation);
	SIZE(WgShaderSourceWGSL, WGPUShaderSourceWGSL); FIELD(WgShaderSourceWGSL, WGPUShaderSourceWGSL, code);
	SIZE(WgShaderModuleDescriptor, WGPUShaderModuleDescriptor); FIELD(WgShaderModuleDescriptor, WGPUShaderModuleDescriptor, label);
	SIZE(WgComputeState, WGPUComputeState);
	FIELD(WgComputeState, WGPUComputeState, module); FIELD(WgComputeState, WGPUComputeState, entryPoint);
	FIELD(WgComputeState, WGPUComputeState, constantCount); FIELD(WgComputeState, WGPUComputeState, constants);
	SIZE(WgComputePipelineDescriptor, WGPUComputePipelineDescriptor);
	FIELD(WgComputePipelineDescriptor, WGPUComputePipelineDescriptor, layout); FIELD(WgComputePipelineDescriptor, WGPUComputePipelineDescriptor, compute);
	SIZE(WgBindGroupEntry, WGPUBindGroupEntry);
	FIELD(WgBindGroupEntry, WGPUBindGroupEntry, binding); FIELD(WgBindGroupEntry, WGPUBindGroupEntry, buffer);
	FIELD(WgBindGroupEntry, WGPUBindGroupEntry, offset); FIELD(WgBindGroupEntry, WGPUBindGroupEntry, size);
	FIELD(WgBindGroupEntry, WGPUBindGroupEntry, sampler); FIELD(WgBindGroupEntry, WGPUBindGroupEntry, textureView);
	SIZE(WgBindGroupDescriptor, WGPUBindGroupDescriptor);
	FIELD(WgBindGroupDescriptor, WGPUBindGroupDescriptor, layout); FIELD(WgBindGroupDescriptor, WGPUBindGroupDescriptor, entryCount);
	FIELD(WgBindGroupDescriptor, WGPUBindGroupDescriptor, entries);
	SAME(WG_STYPE_SHADER_SOURCE_WGSL, WGPUSType_ShaderSourceWGSL);
	SAME(WG_CALLBACK_ALLOW_PROCESS_EVENTS, WGPUCallbackMode_AllowProcessEvents);
	SAME(WG_FEATURE_LEVEL_CORE, WGPUFeatureLevel_Core);
	SAME(WG_STATUS_SUCCESS, WGPURequestAdapterStatus_Success);
	SAME(WG_STATUS_SUCCESS, WGPURequestDeviceStatus_Success);
	SAME(WG_STATUS_SUCCESS, WGPUMapAsyncStatus_Success);
	SAME(WG_BUFFER_MAP_READ, WGPUBufferUsage_MapRead);
	SAME(WG_BUFFER_COPY_SRC, WGPUBufferUsage_CopySrc);
	SAME(WG_BUFFER_COPY_DST, WGPUBufferUsage_CopyDst);
	SAME(WG_BUFFER_UNIFORM, WGPUBufferUsage_Uniform);
	SAME(WG_BUFFER_STORAGE, WGPUBufferUsage_Storage);
	SAME(WG_MAP_READ, WGPUMapMode_Read);
	if (!bad) puts("wgpu89.h matches webgpu.h");
	return bad;
}
