/* wgpu89.h - the part of WebGPU's C API (webgpu.h) that gpu.c calls, written out again in C89: the real header needs
 * C99, and gael loads wgpu-native at run time with dlopen, so only the layout of these structs and the values of these
 * constants have to match it. They are the ones of wgpu-native v29 (webgpu.h of 2026-03); tests/gpu/abi.c checks
 * every size, offset and value here against that header, so a newer wgpu that moves something fails there first. */

#ifndef GAEL_WGPU89_H_
#define GAEL_WGPU89_H_

#include <stddef.h>

typedef void *WgHandle;

typedef struct {
	const char *data;
	size_t length;
} WgStringView;

typedef struct WgChained {
	struct WgChained *next;
	u32 sType;
} WgChained;

typedef struct {
	u64 id;
} WgFuture;

typedef void (*WgRequestCallback)(u32 status, WgHandle got, WgStringView message, void *userdata1, void *userdata2);
typedef void (*WgBufferMapCallback)(u32 status, WgStringView message, void *userdata1, void *userdata2);
typedef void (*WgErrorCallback)(WgHandle const *device, u32 type, WgStringView message, void *userdata1, void *userdata2);
typedef void (*WgLostCallback)(WgHandle const *device, u32 reason, WgStringView message, void *userdata1, void *userdata2);

typedef struct { /* WGPURequestAdapterCallbackInfo and WGPURequestDeviceCallbackInfo */
	WgChained *nextInChain;
	u32 mode;
	WgRequestCallback callback;
	void *userdata1, *userdata2;
} WgRequestCallbackInfo;

typedef struct {
	WgChained *nextInChain;
	u32 mode;
	WgBufferMapCallback callback;
	void *userdata1, *userdata2;
} WgBufferMapCallbackInfo;

typedef struct {
	WgChained *nextInChain;
	WgErrorCallback callback;
	void *userdata1, *userdata2;
} WgUncapturedErrorCallbackInfo;

typedef struct {
	WgChained *nextInChain;
	u32 mode;
	WgLostCallback callback;
	void *userdata1, *userdata2;
} WgDeviceLostCallbackInfo;

typedef struct {
	WgChained *nextInChain;
	u32 featureLevel, powerPreference, forceFallbackAdapter, backendType;
	WgHandle compatibleSurface;
} WgRequestAdapterOptions;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
} WgQueueDescriptor;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	size_t requiredFeatureCount;
	const u32 *requiredFeatures;
	const void *requiredLimits;
	WgQueueDescriptor defaultQueue;
	WgDeviceLostCallbackInfo deviceLostCallbackInfo;
	WgUncapturedErrorCallbackInfo uncapturedErrorCallbackInfo;
} WgDeviceDescriptor;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	u64 usage, size;
	u32 mappedAtCreation;
} WgBufferDescriptor;

typedef struct {
	WgChained chain;
	WgStringView code;
} WgShaderSourceWGSL;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
} WgShaderModuleDescriptor;

typedef struct {
	WgChained *nextInChain;
	WgHandle module;
	WgStringView entryPoint;
	size_t constantCount;
	const void *constants;
} WgComputeState;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	WgHandle layout;
	WgComputeState compute;
} WgComputePipelineDescriptor;

typedef struct {
	WgChained *nextInChain;
	u32 binding;
	WgHandle buffer;
	u64 offset, size;
	WgHandle sampler, textureView;
} WgBindGroupEntry;

typedef struct {
	WgChained *nextInChain;
	WgStringView label;
	WgHandle layout;
	size_t entryCount;
	const WgBindGroupEntry *entries;
} WgBindGroupDescriptor;

#define WG_STYPE_SHADER_SOURCE_WGSL 0x2u
#define WG_CALLBACK_ALLOW_PROCESS_EVENTS 0x2u
#define WG_FEATURE_LEVEL_CORE 0x2u
#define WG_STATUS_SUCCESS 0x1u           /* WGPURequestAdapterStatus, WGPURequestDeviceStatus, WGPUMapAsyncStatus */
#define WG_BUFFER_MAP_READ 0x1u
#define WG_BUFFER_COPY_SRC 0x4u
#define WG_BUFFER_COPY_DST 0x8u
#define WG_BUFFER_UNIFORM 0x40u
#define WG_BUFFER_STORAGE 0x80u
#define WG_MAP_READ 0x1u
#define WG_WHOLE_MAP_SIZE ((size_t)-1)   /* WGPU_WHOLE_MAP_SIZE */

#endif
