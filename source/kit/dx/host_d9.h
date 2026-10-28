// host_d9.h - the Direct3D 9 device's GPU renderer, as the shim calls it.
//
// The shim keeps every resource's bytes in its own storage and decides what
// the GPU needs to hear. The host mirrors textures, surfaces and buffers under
// the shim's object ids and draws what it is told. A texture id names the
// whole texture; a surface of a texture is (texture id, face, level). A
// stand-alone surface - the back buffer, a depth-stencil surface - is a
// one-level texture under its own id.
//
// Weak no-op defaults live in host_d9_default.cpp; host_d9_active() returning
// zero there is what keeps the CPU renderer in charge.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HOST_D9_TEX_2D = 0, HOST_D9_TEX_CUBE = 1, HOST_D9_TEX_VOLUME = 2 };
enum { HOST_D9_USAGE_RENDERTARGET = 1, HOST_D9_USAGE_DEPTH = 2 };

typedef struct HostD9TextureDesc {
    uint32_t id;
    uint32_t kind; // HOST_D9_TEX_*
    uint32_t width, height, levels;
    uint32_t format;  // D3DFORMAT
    uint32_t usage;   // HOST_D9_USAGE_*
    uint32_t samples; // multisampled render target or depth buffer: sample count; 0 or 1 for none
    uint32_t depth;   // volume depth; ignored for 2D/cube textures
} HostD9TextureDesc;

// Non-zero when a GPU renderer is running and takes the device's draws.
int host_d9_active(void);

// (Re)creates the mirror; contents are undefined until uploaded or drawn.
void host_d9_texture_define(const HostD9TextureDesc *desc);
void host_d9_texture_drop(uint32_t id);
// One face/level from the shim's bytes, in the texture's D3DFORMAT.
// For a volume, face is the Z slice at this mip level. Upload completes before
// returning, so the caller may reuse the source bytes for the next slice.
void host_d9_texture_upload(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                            uint32_t pitch);
// Reads a colour face/level back as the texture's own format (32-bit formats
// only). Waits for the GPU. Non-zero on success.
int host_d9_texture_read(uint32_t id, uint32_t face, uint32_t level, uint8_t *bytes,
                         uint32_t pitch);

// A bounded read for D3D9 GetRenderTargetData + LockRect(D3DLOCK_DONOTWAIT): queues a copy of a
// colour face/level (direct 32-bit formats) behind the work already handed to the renderer and
// returns at once. `ticket` (non-zero) names the request; a newer request with the same ticket
// replaces it. poll() never waits for the GPU or the render thread: 1 with the game-size image
// copied into `bytes` (w x h at `pitch`), 0 while the GPU has not run the copy yet, -1 when the
// ticket is unknown or the copy could not be made. forget() drops a ticket's request/result.
void host_d9_texture_read_async(uint32_t id, uint32_t face, uint32_t level, uint32_t ticket);
int host_d9_texture_read_poll(uint32_t ticket, uint8_t *bytes, uint32_t pitch, uint32_t w,
                              uint32_t h);
void host_d9_texture_read_forget(uint32_t ticket);

// Vertex and index buffers: `total` bytes, of which [offset, offset+size)
// changed.
void host_d9_buffer_upload(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                           uint32_t size);
void host_d9_buffer_drop(uint32_t id);

typedef struct HostD9Surface {
    uint32_t id, face, level; // id 0: none
} HostD9Surface;

typedef struct HostD9Target {
    HostD9Surface color[4];
    HostD9Surface depth;
} HostD9Target;

typedef struct HostD9Stream {
    uint32_t buffer; // buffer id, or 0 for the draw's inline vertices
    uint32_t offset, stride;
} HostD9Stream;

// Opt-in guest draw provenance. Addresses are guest values, never host pointers.
// api is the IDirect3DDevice9 vtable slot (81..84). reason: 0 accepted,
// 1 missing shaders, 2 missing declaration, 3 missing IB, 4 invalid inline range.
typedef struct HostD9Admission {
    uint64_t serial;
    uint32_t api, caller, device, reason;
    uint32_t inline_vertices, inline_indices; // guest addresses, UP calls only
    uint32_t resource_identity[9];            // streams 0..7, then index buffer COM identity
} HostD9Admission;
int host_d9_admission_enabled(void);
void host_d9_admission_entry(uint64_t serial);
// Cumulative totals; sampled only after Present has drained the host queue.
// Disjoint producer/consumer fields are read together only at that barrier.
typedef struct HostD9Accounting {
    uint64_t entries, forwarded, rejected, received, received_rejected, unforwarded;
    uint64_t entry_serial_sum, forwarded_serial_sum, received_serial_sum;
    uint64_t last_entry, last_received;
    uint32_t enabled, drained;
} HostD9Accounting;

typedef struct HostD9Draw {
    HostD9Admission admission;
    HostD9Target target;
    int32_t viewport[4]; // x, y, w, h
    float depth_range[2];
    int32_t scissor[4]; // left, top, right, bottom; used when D3DRS_SCISSORTESTENABLE

    const uint8_t *vs;
    uint32_t vs_size;
    const uint8_t *ps;
    uint32_t ps_size;
    uint64_t vs_key, ps_key; // d9sh::code_key of the two programs
    const float *vconst;     // 4 floats each
    uint32_t vconst_count;
    const float *pconst;
    uint32_t pconst_count;
    // Inline so the render queue retains values from this draw, not a later
    // mutation of the device's constants. Nonzero is true, as with D3D BOOL.
    int32_t vbool[16], pbool[16];

    const uint8_t *decl; // D3DVERTEXELEMENT9 records, ending with the 0xff stream
    uint32_t decl_size;
    uint32_t decl_id; // stable key for `decl`
    HostD9Stream stream[8];
    const uint8_t *inline_vertices;
    uint32_t inline_bytes;

    uint32_t index_buffer; // 0: not indexed, unless inline_indices
    const uint8_t *inline_indices;
    uint32_t inline_index_bytes;
    uint32_t index_size; // 2 or 4

    uint32_t primitive; // D3DPRIMITIVETYPE
    uint32_t primitive_count;
    uint32_t start; // first vertex, or first index when indexed
    int32_t base_vertex;

    uint32_t sampler_texture[16];    // texture ids; 0: none
    const uint32_t *sampler_state;   // 16 x 14, indexed [stage * 14 + D3DSAMPLERSTATETYPE]
    const uint32_t *render_state;    // 256, D3DRENDERSTATETYPE
    const uint8_t *render_state_set; // 256 flags: the game set it
    uint32_t projected_mask;         // stages with D3DTTFF_PROJECTED
    // Nonzero: the three state tables above are unchanged for as long as this
    // is, so a host may reuse its copy from an earlier draw with the same value.
    uint64_t state_version;
    const char *label; // what bound the shaders, for diagnostics; may be null
    // Test133: nonzero when stage 0 samples the texture a mod named in private render state 251 as part of the
    // scene (NFSMW: the rear-view mirror target): after the HDR scene marker it keeps the HDR highlight expansion.
    uint32_t hdr_scene_texture;
} HostD9Draw;

void host_d9_draw(const HostD9Draw *draw);
// Diagnostic only: queue in draw order, never render or upload its geometry.
void host_d9_rejected_draw(const HostD9Draw *draw);

// D3DCLEAR_TARGET 1, _ZBUFFER 2, _STENCIL 4. Rects are D3DRECTs.
void host_d9_clear(const HostD9Target *target, const int32_t viewport[4], uint32_t count,
                   const int32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);

// StretchRect: rects are left, top, right, bottom. filter is D3DTEXTUREFILTERTYPE.
void host_d9_stretch(HostD9Surface src, const int32_t src_rect[4], HostD9Surface dst,
                     const int32_t dst_rect[4], uint32_t filter);

// The back buffer is finished: send it to the presenter. Commits the frame's
// GPU work.
void host_d9_present(uint32_t backbuffer, uint32_t width, uint32_t height);

// The last presented frame as tightly packed RGB, for dumps. Waits for the
// GPU. Writes nothing and returns zero when no frame was presented or `cap`
// is too small; *w and *h are set either way when a frame exists.
int host_d9_read_presented(uint8_t *rgb, uint32_t cap, uint32_t *w, uint32_t *h);

// Occlusion queries: the draws between begin and end count the samples that
// pass. result() is 1 with the count once the GPU has run them, 0 while it
// has not, -1 for a query it never saw.
void host_d9_query_begin(uint32_t id);
void host_d9_query_end(uint32_t id);
int host_d9_query_result(uint32_t id, uint32_t *count);
void host_d9_query_drop(uint32_t id);

// Diagnostics: log every draw of the next frame and dump its textures, with
// `tag` in the file names.
void host_d9_probe_next_frame(const char *tag);

#ifdef __cplusplus
}
#endif
