/* gfx.c - drawing on canvases, on the GPU through wgpu (gpu.c): a canvas is an image the GPU draws into; the program
 * draws rects, circles and lines on it, gael batches them, and the GPU draws a batch when the frame is shown (or when
 * the batch is full, or when the program reads a pixel). One pipeline draws everything: each shape is two triangles,
 * and a circle's fragment keeps the disc inside its square, smoothed over a pixel at the edge.
 *
 *   (new-canvas width height)             an image of that many pixels, black and clear; its number
 *   (draw-clear c color)                  the whole canvas one color, 0xRRGGBBAA
 *   (draw-rect c x y width height color)  x and y the top left, in pixels from the top left of the canvas
 *   (draw-circle c x y radius color)
 *   (draw-line c x1 y1 x2 y2 width color)
 *   (show-frame c)                        draws what is batched; true while the canvas is there to draw on
 *   (canvas-pixel c x y)                  a pixel as 0xRRGGBBAA
 *   (save-png c path)                     the canvas as a PNG file; true when it was written
 *
 * Coordinates are floats or f32s. Without a GPU every one of these is a runtime error: a canvas only exists there. */

#define MAX_CANVAS 16
#define GFX_SHAPES 16384
#define GFX_VERTS (GFX_SHAPES * 6)

typedef struct {
	f32 x, y, u, v;
	u32 rgba; /* bytes r, g, b, a in that order: the GPU reads it as unorm8x4 */
	f32 kind; /* 0 a solid shape, 1 a circle's square */
} GVert;

static struct {
	int used, w, h;
	WgHandle tex, view, ubuf, bg;
	int clear_pending; /* a draw-clear waits for the next batch */
	u32 clear;
} g_canvas[MAX_CANVAS];

static GVert g_gv[GFX_VERTS];
static i32 g_ngv, g_gv_canvas = -1; /* the batch, and the canvas it is for */
static WgHandle g_gfx_module, g_gfx_pipeline, g_gfx_layout, g_gfx_vbuf;

static const char *const g_gfx_wgsl[] = { /* C89 keeps a string literal under 510 bytes: the lines are joined at the start */
	"struct U { size: vec2<f32> }\n",
	"@group(0) @binding(0) var<uniform> u: U;\n",
	"struct V { @location(0) pos: vec2<f32>, @location(1) uv: vec2<f32>, @location(2) color: vec4<f32>, @location(3) kind: f32 }\n",
	"struct O { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32>, @location(1) color: vec4<f32>, @location(2) kind: f32 }\n",
	"@vertex fn vs(v: V) -> O {\n",
	"  var o: O;\n",
	"  o.pos = vec4<f32>(v.pos.x / u.size.x * 2.0 - 1.0, 1.0 - v.pos.y / u.size.y * 2.0, 0.0, 1.0);\n",
	"  o.uv = v.uv;\n",
	"  o.color = v.color;\n",
	"  o.kind = v.kind;\n",
	"  return o;\n",
	"}\n",
	"@fragment fn fs(i: O) -> @location(0) vec4<f32> {\n",
	"  var c = i.color;\n",
	"  if (i.kind > 0.5) {\n",
	"    let d = length(i.uv);\n",
	"    let w = max(fwidth(d), 0.0001);\n",
	"    c.a = c.a * clamp((1.0 - d) / w + 0.5, 0.0, 1.0);\n",
	"    if (c.a <= 0.0) { discard; }\n",
	"  }\n",
	"  return c;\n",
	"}\n",
	0
};

/* gfx_ready: the GPU and the one pipeline everything is drawn with; 0 with the reason in g_gpu_error */
static int gfx_ready(void)
{
	WgShaderSourceWGSL src;
	WgShaderModuleDescriptor md;
	WgRenderPipelineDescriptor pd;
	WgVertexAttribute attrs[4];
	WgVertexBufferLayout vbl;
	WgFragmentState fs;
	WgColorTargetState ct;
	WgBlendState blend;
	WgBufferDescriptor bd;
	int errors, k;
	if (g_gfx_pipeline) return 1;
	if (!gpu_ready()) {
		sprintf(g_gpu_error, "drawing needs a GPU, and there is none (%.400s)", g_gpu.why);
		return 0;
	}
	errors = g_gpu.errors;
	memset(&src, 0, sizeof(src));
	memset(&md, 0, sizeof(md));
	{
		static char text[2048];
		size_t at = 0;
		for (k = 0; g_gfx_wgsl[k]; k++) kt_add(text, sizeof(text), &at, g_gfx_wgsl[k]);
		src.code = wg_str(text);
	}
	src.chain.sType = WG_STYPE_SHADER_SOURCE_WGSL;
	md.nextInChain = &src.chain;
	g_gfx_module = g_gpu.wgpuDeviceCreateShaderModule(g_gpu.device, &md);
	memset(attrs, 0, sizeof(attrs));
	attrs[0].format = WG_VERTEX_FLOAT32X2;
	attrs[0].offset = 0;
	attrs[0].shaderLocation = 0;
	attrs[1].format = WG_VERTEX_FLOAT32X2;
	attrs[1].offset = 8;
	attrs[1].shaderLocation = 1;
	attrs[2].format = WG_VERTEX_UNORM8X4;
	attrs[2].offset = 16;
	attrs[2].shaderLocation = 2;
	attrs[3].format = WG_VERTEX_FLOAT32;
	attrs[3].offset = 20;
	attrs[3].shaderLocation = 3;
	memset(&vbl, 0, sizeof(vbl));
	vbl.stepMode = WG_VERTEX_STEP_VERTEX;
	vbl.arrayStride = sizeof(GVert);
	vbl.attributeCount = 4;
	vbl.attributes = attrs;
	memset(&blend, 0, sizeof(blend)); /* the usual alpha blend */
	blend.color.operation = WG_BLEND_ADD;
	blend.color.srcFactor = WG_BLEND_SRC_ALPHA;
	blend.color.dstFactor = WG_BLEND_ONE_MINUS_SRC_ALPHA;
	blend.alpha.operation = WG_BLEND_ADD;
	blend.alpha.srcFactor = WG_BLEND_ONE;
	blend.alpha.dstFactor = WG_BLEND_ONE_MINUS_SRC_ALPHA;
	memset(&ct, 0, sizeof(ct));
	ct.format = WG_FORMAT_RGBA8_UNORM;
	ct.blend = &blend;
	ct.writeMask = WG_COLOR_WRITE_ALL;
	memset(&fs, 0, sizeof(fs));
	fs.module = g_gfx_module;
	fs.entryPoint = wg_str("fs");
	fs.targetCount = 1;
	fs.targets = &ct;
	memset(&pd, 0, sizeof(pd));
	pd.vertex.module = g_gfx_module;
	pd.vertex.entryPoint = wg_str("vs");
	pd.vertex.bufferCount = 1;
	pd.vertex.buffers = &vbl;
	pd.primitive.topology = WG_TOPOLOGY_TRIANGLE_LIST;
	pd.primitive.frontFace = WG_FRONT_CCW;
	pd.primitive.cullMode = WG_CULL_NONE;
	pd.multisample.count = 1;
	pd.multisample.mask = 0xFFFFFFFFu;
	pd.fragment = &fs;
	if (g_gfx_module) g_gfx_pipeline = g_gpu.wgpuDeviceCreateRenderPipeline(g_gpu.device, &pd);
	if (g_gfx_pipeline) g_gfx_layout = g_gpu.wgpuRenderPipelineGetBindGroupLayout(g_gfx_pipeline, 0);
	memset(&bd, 0, sizeof(bd));
	bd.size = (u64)sizeof(g_gv);
	bd.usage = WG_BUFFER_VERTEX | WG_BUFFER_COPY_DST;
	if (g_gfx_layout) g_gfx_vbuf = g_gpu.wgpuDeviceCreateBuffer(g_gpu.device, &bd);
	if (!g_gfx_vbuf || g_gpu.errors != errors) {
		sprintf(g_gpu_error, "the GPU did not take gael's drawing pipeline: %.400s", g_gpu.why);
		g_gfx_pipeline = 0; /* (what was made stays: this happens once, and is gael's mistake) */
		return 0;
	}
	return 1;
}

/* gfx_canvas_new W H: a canvas; its number, or -1 with the reason in g_gpu_error */
static i32 gfx_canvas_new(i64 w, i64 h)
{
	WgTextureDescriptor td;
	WgBufferDescriptor bd;
	WgBindGroupEntry e;
	WgBindGroupDescriptor bgd;
	f32 size[4];
	i32 c;
	int errors;
	if (!gfx_ready()) return -1;
	if (w < 1 || h < 1 || w > 8192 || h > 8192) {
		sprintf(g_gpu_error, "new-canvas: %ldx%ld pixels; a canvas is 1 to 8192 pixels each way", (long)w, (long)h);
		return -1;
	}
	for (c = 0; c < MAX_CANVAS && g_canvas[c].used; c++) {}
	if (c == MAX_CANVAS) {
		sprintf(g_gpu_error, "new-canvas: there are %d canvases already, all there can be", MAX_CANVAS);
		return -1;
	}
	errors = g_gpu.errors;
	memset(&g_canvas[c], 0, sizeof(g_canvas[c]));
	memset(&td, 0, sizeof(td));
	td.usage = WG_TEXTURE_RENDER_ATTACHMENT | WG_TEXTURE_COPY_SRC;
	td.dimension = WG_TEXTURE_DIMENSION_2D;
	td.size.width = (u32)w;
	td.size.height = (u32)h;
	td.size.depthOrArrayLayers = 1;
	td.format = WG_FORMAT_RGBA8_UNORM;
	td.mipLevelCount = 1;
	td.sampleCount = 1;
	g_canvas[c].tex = g_gpu.wgpuDeviceCreateTexture(g_gpu.device, &td);
	if (g_canvas[c].tex) g_canvas[c].view = g_gpu.wgpuTextureCreateView(g_canvas[c].tex, 0);
	memset(&bd, 0, sizeof(bd));
	bd.size = 16;
	bd.usage = WG_BUFFER_UNIFORM | WG_BUFFER_COPY_DST;
	g_canvas[c].ubuf = g_gpu.wgpuDeviceCreateBuffer(g_gpu.device, &bd);
	size[0] = (f32)w;
	size[1] = (f32)h;
	size[2] = size[3] = 0;
	if (g_canvas[c].ubuf) g_gpu.wgpuQueueWriteBuffer(g_gpu.queue, g_canvas[c].ubuf, 0, size, 16);
	memset(&e, 0, sizeof(e));
	e.binding = 0;
	e.buffer = g_canvas[c].ubuf;
	e.size = 16;
	memset(&bgd, 0, sizeof(bgd));
	bgd.layout = g_gfx_layout;
	bgd.entryCount = 1;
	bgd.entries = &e;
	if (g_canvas[c].ubuf) g_canvas[c].bg = g_gpu.wgpuDeviceCreateBindGroup(g_gpu.device, &bgd);
	if (!g_canvas[c].view || !g_canvas[c].bg || g_gpu.errors != errors) {
		sprintf(g_gpu_error, "new-canvas: the GPU made no %ldx%ld image: %.300s", (long)w, (long)h, g_gpu.why);
		return -1;
	}
	g_canvas[c].used = 1;
	g_canvas[c].w = (int)w;
	g_canvas[c].h = (int)h;
	g_canvas[c].clear_pending = 1; /* a new canvas starts clear, black */
	g_canvas[c].clear = 0;
	return c;
}

static int gfx_canvas(i64 c) /* a canvas there is; 0 with the error said */
{
	if (c >= 0 && c < MAX_CANVAS && g_canvas[c].used) return 1;
	sprintf(g_gpu_error, "there is no canvas %ld: new-canvas gives one", (long)c);
	return 0;
}

/* gfx_flush C: draw the batch (and a waiting clear) into canvas C; 0 with the error said */
static int gfx_flush(i32 c)
{
	WgRenderPassColorAttachment ca;
	WgRenderPassDescriptor rp;
	WgHandle enc, pass, cmd;
	int errors = g_gpu.errors;
	i32 n = g_gv_canvas == c ? g_ngv : 0;
	if (!n && !g_canvas[c].clear_pending) return 1;
	if (n) g_gpu.wgpuQueueWriteBuffer(g_gpu.queue, g_gfx_vbuf, 0, g_gv, (size_t)n * sizeof(GVert));
	memset(&ca, 0, sizeof(ca));
	ca.view = g_canvas[c].view;
	ca.depthSlice = WG_DEPTH_SLICE_UNDEFINED;
	ca.loadOp = g_canvas[c].clear_pending ? WG_LOAD_CLEAR : WG_LOAD_LOAD;
	ca.storeOp = WG_STORE_STORE;
	ca.clearValue.r = (f64)(g_canvas[c].clear >> 24 & 255) / 255.0;
	ca.clearValue.g = (f64)(g_canvas[c].clear >> 16 & 255) / 255.0;
	ca.clearValue.b = (f64)(g_canvas[c].clear >> 8 & 255) / 255.0;
	ca.clearValue.a = (f64)(g_canvas[c].clear & 255) / 255.0;
	memset(&rp, 0, sizeof(rp));
	rp.colorAttachmentCount = 1;
	rp.colorAttachments = &ca;
	enc = g_gpu.wgpuDeviceCreateCommandEncoder(g_gpu.device, 0);
	pass = g_gpu.wgpuCommandEncoderBeginRenderPass(enc, &rp);
	if (n) {
		g_gpu.wgpuRenderPassEncoderSetPipeline(pass, g_gfx_pipeline);
		g_gpu.wgpuRenderPassEncoderSetBindGroup(pass, 0, g_canvas[c].bg, 0, 0);
		g_gpu.wgpuRenderPassEncoderSetVertexBuffer(pass, 0, g_gfx_vbuf, 0, (u64)n * sizeof(GVert));
		g_gpu.wgpuRenderPassEncoderDraw(pass, (u32)n, 1, 0, 0);
	}
	g_gpu.wgpuRenderPassEncoderEnd(pass);
	g_gpu.wgpuRenderPassEncoderRelease(pass);
	cmd = g_gpu.wgpuCommandEncoderFinish(enc, 0);
	if (g_gpu.errors != errors) { /* wgpu would stop the program at the submit */
		sprintf(g_gpu_error, "the GPU did not take a frame: %.400s", g_gpu.why);
		return 0;
	}
	g_gpu.wgpuQueueSubmit(g_gpu.queue, 1, &cmd);
	g_gpu.wgpuCommandBufferRelease(cmd);
	g_gpu.wgpuCommandEncoderRelease(enc);
	g_canvas[c].clear_pending = 0;
	if (g_gv_canvas == c) g_ngv = 0;
	return 1;
}

static u32 gfx_rgba(i64 color) /* 0xRRGGBBAA as the bytes r, g, b, a in memory */
{
	u32 c = (u32)color;
	return (c >> 24 & 255) | (c >> 16 & 255) << 8 | (c >> 8 & 255) << 16 | (c & 255) << 24;
}

/* gfx_quad C P KIND COLOR: two triangles through the four corners P (x y pairs, in order around), with uv -1..1 for a
 * circle's square; 0 with the error said */
static int gfx_quad(i32 c, const f32 *p, f32 kind, i64 color)
{
	static const f32 uv[8] = { -1, -1, 1, -1, 1, 1, -1, 1 };
	static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
	u32 rgba = gfx_rgba(color);
	int k;
	if (g_gv_canvas != c && g_ngv && !gfx_flush(g_gv_canvas)) return 0;
	if (g_ngv + 6 > GFX_VERTS && !gfx_flush(c)) return 0;
	g_gv_canvas = c;
	for (k = 0; k < 6; k++) {
		GVert *v = &g_gv[g_ngv++];
		v->x = p[tri[k] * 2];
		v->y = p[tri[k] * 2 + 1];
		v->u = uv[tri[k] * 2];
		v->v = uv[tri[k] * 2 + 1];
		v->rgba = rgba;
		v->kind = kind;
	}
	return 1;
}

/* gfx_read C X Y W H TO: the pixels of a part of canvas C, row by row, 4 bytes each, into TO (or a PNG writer's rows);
 * the batch is drawn first. 0 with the error said */
static int gfx_read(i32 c, u32 x, u32 y, u32 w, u32 h, int (*row)(void *, const unsigned char *, u32), void *to)
{
	WgBufferDescriptor bd;
	WgTexelCopyTextureInfo src;
	WgTexelCopyBufferInfo dst;
	WgExtent3D size;
	WgBufferMapCallbackInfo mi;
	WgHandle buf, enc, cmd;
	u32 stride = (w * 4 + 255) / 256 * 256, r;
	const unsigned char *got;
	int ok = 1, errors, k;
	if (!gfx_flush(c)) return 0;
	errors = g_gpu.errors;
	memset(&bd, 0, sizeof(bd));
	bd.size = (u64)stride * h;
	bd.usage = WG_BUFFER_MAP_READ | WG_BUFFER_COPY_DST;
	if (!(buf = g_gpu.wgpuDeviceCreateBuffer(g_gpu.device, &bd))) {
		sprintf(g_gpu_error, "the GPU gave no buffer to read the canvas into: %.300s", g_gpu.why);
		return 0;
	}
	memset(&src, 0, sizeof(src));
	src.texture = g_canvas[c].tex;
	src.origin.x = x;
	src.origin.y = y;
	src.aspect = WG_ASPECT_ALL;
	memset(&dst, 0, sizeof(dst));
	dst.layout.bytesPerRow = stride;
	dst.layout.rowsPerImage = h;
	dst.buffer = buf;
	size.width = w;
	size.height = h;
	size.depthOrArrayLayers = 1;
	enc = g_gpu.wgpuDeviceCreateCommandEncoder(g_gpu.device, 0);
	g_gpu.wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &size);
	cmd = g_gpu.wgpuCommandEncoderFinish(enc, 0);
	if (g_gpu.errors != errors) {
		sprintf(g_gpu_error, "the GPU did not copy the canvas: %.300s", g_gpu.why);
		g_gpu.wgpuBufferRelease(buf);
		return 0;
	}
	g_gpu.wgpuQueueSubmit(g_gpu.queue, 1, &cmd);
	g_gpu.wgpuCommandBufferRelease(cmd);
	g_gpu.wgpuCommandEncoderRelease(enc);
	memset(&mi, 0, sizeof(mi));
	mi.mode = WG_CALLBACK_ALLOW_PROCESS_EVENTS;
	mi.callback = on_map;
	g_mapped = 0;
	g_gpu.wgpuBufferMapAsync(buf, WG_MAP_READ, 0, (size_t)bd.size, mi);
	for (k = 0; !g_mapped && k < 1000000; k++) {
		g_gpu.wgpuDevicePoll(g_gpu.device, 1, 0);
		g_gpu.wgpuInstanceProcessEvents(g_gpu.inst);
	}
	if (g_mapped <= 0 || !(got = (const unsigned char *)g_gpu.wgpuBufferGetConstMappedRange(buf, 0, (size_t)bd.size))) {
		sprintf(g_gpu_error, "the canvas did not come back from the GPU: %.300s", g_gpu.why);
		ok = 0;
	} else {
		for (r = 0; r < h && ok; r++) ok = row(to, got + (size_t)r * stride, w);
		g_gpu.wgpuBufferUnmap(buf);
	}
	g_gpu.wgpuBufferRelease(buf);
	return ok;
}

static int gfx_pixel_row(void *to, const unsigned char *p, u32 w)
{
	(void)w;
	*(u32 *)to = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3];
	return 1;
}

/* PNG, as plain as it gets: the rows behind a filter byte of 0, in deflate's stored blocks, no compression */
static u32 g_crc_table[256];

static u32 png_crc(u32 crc, const unsigned char *p, size_t n)
{
	size_t i;
	if (!g_crc_table[1]) {
		u32 k, b, v;
		for (k = 0; k < 256; k++) {
			for (v = k, b = 0; b < 8; b++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
			g_crc_table[k] = v;
		}
	}
	for (i = 0; i < n; i++) crc = g_crc_table[(crc ^ p[i]) & 255] ^ (crc >> 8);
	return crc;
}

typedef struct {
	FILE *f;
	u32 crc, adler_a, adler_b, left; /* the CRC of the IDAT chunk so far, Adler-32, the bytes left in this block */
	int ok;
} PngOut;

static o png_be32(unsigned char *p, u32 v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static o png_put(PngOut *po, const unsigned char *p, size_t n) /* bytes of the IDAT chunk's data */
{
	po->crc = png_crc(po->crc, p, n);
	if (fwrite(p, 1, n, po->f) != n) po->ok = 0;
}

static int png_row(void *to, const unsigned char *p, u32 w)
{
	PngOut *po = (PngOut *)to;
	unsigned char head[5], filter = 0;
	u32 n = w * 4 + 1, i;
	/* each row is one stored block: 5 header bytes, then the row; the last row's block is marked final */
	head[0] = (unsigned char)(po->left == 1 ? 1 : 0);
	head[1] = (unsigned char)(n & 255);
	head[2] = (unsigned char)(n >> 8);
	head[3] = (unsigned char)(~n & 255);
	head[4] = (unsigned char)((~n >> 8) & 255);
	png_put(po, head, 5);
	png_put(po, &filter, 1);
	png_put(po, p, (size_t)w * 4);
	po->adler_a = (po->adler_a + 0) % 65521; /* the filter byte */
	po->adler_b = (po->adler_b + po->adler_a) % 65521;
	for (i = 0; i < w * 4; i++) {
		po->adler_a = (po->adler_a + p[i]) % 65521;
		po->adler_b = (po->adler_b + po->adler_a) % 65521;
	}
	po->left--;
	return po->ok;
}

/* gfx_save C PATH: canvas C as a PNG; 1 when it was written */
static int gfx_save(i32 c, const char *path)
{
	static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
	unsigned char b[32];
	u32 w = (u32)g_canvas[c].w, h = (u32)g_canvas[c].h, len;
	PngOut po;
	int ok;
	if (!(po.f = fopen(path, "wb"))) return 0;
	po.ok = fwrite(sig, 1, 8, po.f) == 8;
	png_be32(b, 13); /* IHDR: the size, 8 bits, RGBA, no interlace */
	memcpy(b + 4, "IHDR", 4);
	png_be32(b + 8, w);
	png_be32(b + 12, h);
	b[16] = 8;
	b[17] = 6;
	b[18] = b[19] = b[20] = 0;
	png_be32(b + 21, png_crc(0xffffffffu, b + 4, 17) ^ 0xffffffffu);
	if (fwrite(b, 1, 25, po.f) != 25) po.ok = 0;
	len = 2 + h * (5 + 1 + w * 4) + 4; /* the zlib header, a block per row, Adler-32 */
	png_be32(b, len);
	memcpy(b + 4, "IDAT", 4);
	if (fwrite(b, 1, 8, po.f) != 8) po.ok = 0;
	po.crc = png_crc(0xffffffffu, b + 4, 4);
	b[0] = 0x78; /* zlib: deflate, a 32K window, no dictionary */
	b[1] = 0x01;
	png_put(&po, b, 2);
	po.adler_a = 1;
	po.adler_b = 0;
	po.left = h;
	ok = gfx_read(c, 0, 0, w, h, png_row, &po);
	png_be32(b, po.adler_b << 16 | po.adler_a);
	png_put(&po, b, 4);
	png_be32(b, po.crc ^ 0xffffffffu);
	png_be32(b + 4, 0); /* IEND */
	memcpy(b + 8, "IEND", 4);
	png_be32(b + 12, png_crc(0xffffffffu, b + 8, 4) ^ 0xffffffffu);
	if (fwrite(b, 1, 16, po.f) != 16) po.ok = 0;
	if (fclose(po.f)) po.ok = 0;
	return ok && po.ok;
}

static f32 gfx_f(Cell c) { return (f32)c.f; }

/* gpu_close: what the program made on the GPU, and the GPU itself, given back when it ends - also through (exit n) */
static void gpu_close(void)
{
	i32 k;
	if (!g_gpu.ok) return;
	for (k = 0; k < MAX_KERNELS; k++) gpu_forget(k);
	for (k = 0; k < MAX_CANVAS; k++) {
		if (!g_canvas[k].used) continue;
		if (g_canvas[k].bg) g_gpu.wgpuBindGroupRelease(g_canvas[k].bg);
		if (g_canvas[k].ubuf) g_gpu.wgpuBufferRelease(g_canvas[k].ubuf);
		if (g_canvas[k].view) g_gpu.wgpuTextureViewRelease(g_canvas[k].view);
		if (g_canvas[k].tex) g_gpu.wgpuTextureRelease(g_canvas[k].tex);
		g_canvas[k].used = 0;
	}
	if (g_gfx_vbuf) g_gpu.wgpuBufferRelease(g_gfx_vbuf);
	if (g_gfx_layout) g_gpu.wgpuBindGroupLayoutRelease(g_gfx_layout);
	if (g_gfx_pipeline) g_gpu.wgpuRenderPipelineRelease(g_gfx_pipeline);
	if (g_gfx_module) g_gpu.wgpuShaderModuleRelease(g_gfx_module);
	g_gfx_vbuf = g_gfx_layout = g_gfx_pipeline = g_gfx_module = 0;
	g_gpu.wgpuQueueRelease(g_gpu.queue);
	g_gpu.wgpuDeviceRelease(g_gpu.device);
	g_gpu.wgpuAdapterRelease(g_gpu.adapter);
	g_gpu.wgpuInstanceRelease(g_gpu.inst);
	g_gpu.ok = 0;
}

/* gfx_op OP ARGS OUT: the drawing builtin OP (its a) on its arguments; 1 with a value in OUT, 0 without one, -1 an error
 * in g_gpu_error. The path of save-png is a string the caller lets go of */
static int gfx_op(i32 op, const Cell *a, Cell *out, const char *path)
{
	f32 p[8];
	i32 c;
	if (op == 0) { /* new-canvas */
		if ((c = gfx_canvas_new(a[0].i, a[1].i)) < 0) return -1;
		out->i = c;
		return 1;
	}
	if (!gfx_canvas(a[0].i)) return -1;
	c = (i32)a[0].i;
	switch (op) {
	case 1: /* draw-clear: what is batched for it goes, the clear comes first */
		if (g_gv_canvas == c) g_ngv = 0;
		g_canvas[c].clear_pending = 1;
		g_canvas[c].clear = (u32)a[1].i;
		return 0;
	case 2: /* draw-rect */
		p[0] = gfx_f(a[1]);
		p[1] = gfx_f(a[2]);
		p[2] = p[0] + gfx_f(a[3]);
		p[3] = p[1];
		p[4] = p[2];
		p[5] = p[1] + gfx_f(a[4]);
		p[6] = p[0];
		p[7] = p[5];
		return gfx_quad(c, p, 0, a[5].i) ? 0 : -1;
	case 3: { /* draw-circle */
		f32 x = gfx_f(a[1]), y = gfx_f(a[2]), r = gfx_f(a[3]);
		p[0] = x - r;
		p[1] = y - r;
		p[2] = x + r;
		p[3] = y - r;
		p[4] = x + r;
		p[5] = y + r;
		p[6] = x - r;
		p[7] = y + r;
		return gfx_quad(c, p, 1, a[4].i) ? 0 : -1;
	}
	case 4: { /* draw-line: a rect along it, width across */
		f32 x1 = gfx_f(a[1]), y1 = gfx_f(a[2]), x2 = gfx_f(a[3]), y2 = gfx_f(a[4]), hw = gfx_f(a[5]) / 2;
		f32 dx = x2 - x1, dy = y2 - y1, l = (f32)sqrt((f64)(dx * dx + dy * dy)), nx, ny;
		if (l == 0) return 0;
		nx = -dy / l * hw;
		ny = dx / l * hw;
		p[0] = x1 + nx;
		p[1] = y1 + ny;
		p[2] = x2 + nx;
		p[3] = y2 + ny;
		p[4] = x2 - nx;
		p[5] = y2 - ny;
		p[6] = x1 - nx;
		p[7] = y1 - ny;
		return gfx_quad(c, p, 0, a[6].i) ? 0 : -1;
	}
	case 5: /* show-frame */
		if (!gfx_flush(c)) return -1;
		out->i = 1;
		return 1;
	case 6: { /* canvas-pixel */
		u32 v = 0;
		if (a[1].i < 0 || a[2].i < 0 || a[1].i >= g_canvas[c].w || a[2].i >= g_canvas[c].h) {
			sprintf(g_gpu_error, "canvas-pixel: %ld,%ld is outside the canvas, %dx%d", (long)a[1].i, (long)a[2].i, g_canvas[c].w, g_canvas[c].h);
			return -1;
		}
		if (!gfx_read(c, (u32)a[1].i, (u32)a[2].i, 1, 1, gfx_pixel_row, &v)) return -1;
		out->i = (i64)v;
		return 1;
	}
	default: /* save-png */
		out->i = path && gfx_save(c, path);
		return 1;
	}
}
