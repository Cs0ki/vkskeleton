/*
 * Experimental native-Vulkan prototype for Sven Co-op.
 *
 * It captures the real X11 window from Xlib, creates a Vulkan swapchain on
 * that window, emulates the small fixed-function/matrix subset used by hw.so
 * on the CPU, and feeds immediate-mode geometry through a real Vulkan
 * graphics pipeline. It is not a complete renderer.
 *
 * Disabled unless VKSKELETON_ENABLE=1 is set.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "shaders_spirv.h"
#include "vkx.h"

typedef void *(*dlsym_t)(void *, const char *);
typedef __GLXextFuncPtr (*getproc_t)(const GLubyte *);
typedef void (*fn_gl_clear)(GLbitfield);
typedef void (*fn_gl_clear_color)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void (*fn_sdl_swap_window)(void *);

typedef Window (*fn_x_create_window)(Display *, Window, int, int, unsigned int,
                                     unsigned int, unsigned int, int,
                                     unsigned int, Visual *, unsigned long,
                                     XSetWindowAttributes *);
typedef Window (*fn_x_create_simple_window)(Display *, Window, int, int,
                                            unsigned int, unsigned int,
                                            unsigned int, unsigned long,
                                            unsigned long);
typedef int (*fn_x_map_window)(Display *, Window);
typedef int (*fn_x_map_raised)(Display *, Window);

/* Real GL entry points we forward to while tracking state. */
typedef void (*fn_gl_begin)(GLenum);
typedef void (*fn_gl_end)(void);
typedef void (*fn_gl_vertex2f)(GLfloat, GLfloat);
typedef void (*fn_gl_vertex3f)(GLfloat, GLfloat, GLfloat);
typedef void (*fn_gl_vertex3fv)(const GLfloat *);
typedef void (*fn_gl_vertex3i)(GLint, GLint, GLint);
typedef void (*fn_gl_texcoord2f)(GLfloat, GLfloat);
typedef void (*fn_gl_color3f)(GLfloat, GLfloat, GLfloat);
typedef void (*fn_gl_color4f)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*fn_gl_color3ubv)(const GLubyte *);
typedef void (*fn_gl_color4ub)(GLubyte, GLubyte, GLubyte, GLubyte);
typedef void (*fn_gl_color4ubv)(const GLubyte *);
typedef void (*fn_gl_matrix_mode)(GLenum);
typedef void (*fn_gl_void)(void);
typedef void (*fn_gl_translate)(GLfloat, GLfloat, GLfloat);
typedef void (*fn_gl_rotate)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*fn_gl_ortho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble,
                            GLdouble);
typedef fn_gl_ortho fn_gl_frustum;
typedef void (*fn_gl_load_matrix)(const GLfloat *);
typedef void (*fn_gl_enable)(GLenum);
typedef void (*fn_gl_disable)(GLenum);
typedef void (*fn_gl_bind_texture)(GLenum, GLuint);
typedef void (*fn_gl_tex_image2d)(GLenum, GLint, GLint, GLsizei, GLsizei,
                                  GLint, GLenum, GLenum, const void *);
typedef void (*fn_gl_tex_sub_image2d)(GLenum, GLint, GLint, GLint, GLsizei,
                                      GLsizei, GLenum, GLenum, const void *);
typedef void (*fn_gl_pixel_storei)(GLenum, GLint);
typedef void (*fn_gl_tex_parameteri)(GLenum, GLenum, GLint);
typedef void (*fn_gl_tex_parameterf)(GLenum, GLenum, GLfloat);
typedef void (*fn_gl_alpha_func)(GLenum, GLfloat);
typedef void (*fn_gl_tex_envf)(GLenum, GLenum, GLfloat);
typedef void (*fn_gl_blend_func)(GLenum, GLenum);
typedef void (*fn_gl_client_state)(GLenum);
typedef void (*fn_gl_array_pointer)(GLint, GLenum, GLsizei, const void *);
typedef void (*fn_gl_draw_arrays)(GLenum, GLint, GLsizei);
typedef void (*fn_gl_bind_framebuffer)(GLenum, GLuint);
typedef void (*fn_gl_active_texture)(GLenum);
typedef void (*fn_gl_multi_texcoord2f)(GLenum, GLfloat, GLfloat);
typedef void (*fn_gl_multi_texcoord2fv)(GLenum, const GLfloat *);
typedef void (*fn_gl_tex_envi)(GLenum, GLenum, GLint);

static dlsym_t real_dlsym;
static getproc_t real_getproc;
static getproc_t real_getproc_arb;
static fn_gl_clear real_gl_clear;
static fn_gl_clear_color real_gl_clear_color;
static fn_sdl_swap_window real_sdl_swap_window;
static fn_x_create_window real_x_create_window;
static fn_x_create_simple_window real_x_create_simple_window;
static fn_x_map_window real_x_map_window;
static fn_x_map_raised real_x_map_raised;
static fn_gl_begin real_gl_begin;
static fn_gl_end real_gl_end;
static fn_gl_vertex2f real_gl_vertex2f;
static fn_gl_vertex3f real_gl_vertex3f;
static fn_gl_vertex3fv real_gl_vertex3fv;
static fn_gl_vertex3i real_gl_vertex3i;
static fn_gl_texcoord2f real_gl_texcoord2f;
static fn_gl_color3f real_gl_color3f;
static fn_gl_color4f real_gl_color4f;
static fn_gl_color3ubv real_gl_color3ubv;
static fn_gl_color4ub real_gl_color4ub;
static fn_gl_color4ubv real_gl_color4ubv;
static fn_gl_matrix_mode real_gl_matrix_mode;
static fn_gl_void real_gl_load_identity;
static fn_gl_void real_gl_push_matrix;
static fn_gl_void real_gl_pop_matrix;
static fn_gl_translate real_gl_translatef;
static fn_gl_rotate real_gl_rotatef;
static fn_gl_translate real_gl_scalef;
static fn_gl_ortho real_gl_ortho;
static fn_gl_ortho real_gl_frustum;
static fn_gl_load_matrix real_gl_load_matrixf;
static fn_gl_enable real_gl_enable;
static fn_gl_disable real_gl_disable;
static fn_gl_bind_texture real_gl_bind_texture;
static fn_gl_tex_image2d real_gl_tex_image2d;
static fn_gl_tex_sub_image2d real_gl_tex_sub_image2d;
static fn_gl_pixel_storei real_gl_pixel_storei;
static fn_gl_tex_parameteri real_gl_tex_parameteri;
static fn_gl_tex_parameterf real_gl_tex_parameterf;
static fn_gl_alpha_func real_gl_alpha_func;
static fn_gl_tex_envf real_gl_tex_envf;
static fn_gl_blend_func real_gl_blend_func;
static fn_gl_client_state real_gl_enable_client_state;
static fn_gl_client_state real_gl_disable_client_state;
static fn_gl_array_pointer real_gl_vertex_pointer;
static fn_gl_array_pointer real_gl_tex_coord_pointer;
static fn_gl_draw_arrays real_gl_draw_arrays;
static fn_gl_bind_framebuffer real_gl_bind_framebuffer;
static fn_gl_active_texture real_gl_active_texture;
static fn_gl_active_texture real_gl_active_texture_arb;
static fn_gl_multi_texcoord2f real_gl_multi_texcoord2f;
static fn_gl_multi_texcoord2f real_gl_multi_texcoord2f_arb;
static fn_gl_multi_texcoord2fv real_gl_multi_texcoord2fv;
static fn_gl_multi_texcoord2fv real_gl_multi_texcoord2fv_arb;
static fn_gl_tex_envi real_gl_tex_envi;

#define MAX_TRACKED_WINDOWS 16
typedef struct {
    Display *dpy;
    Window window;
    Window parent;
    unsigned int width;
    unsigned int height;
    int mapped;
} tracked_window_t;

static tracked_window_t tracked[MAX_TRACKED_WINDOWS];
static int tracked_count;

static void *vulkan_lib;
static PFN_vkGetInstanceProcAddr fp_vkGetInstanceProcAddr;
static PFN_vkGetDeviceProcAddr fp_vkGetDeviceProcAddr;
static VkInstance instance;
static VkPhysicalDevice physdev;
static VkDevice device;
static VkQueue queue;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkFormat swap_format;
static VkExtent2D swap_extent;
static VkImage swap_images[16];
static uint32_t swap_image_count;
static VkImageView image_views[16];
#define VK_DEPTH_FORMAT VK_FORMAT_D24_UNORM_S8_UINT
static VkImage depth_image;
static VkDeviceMemory depth_memory;
static VkImageView depth_view;
static VkFramebuffer framebuffers[16];
static VkRenderPass render_pass;
static VkShaderModule vert_module;
static VkShaderModule frag_module;
static VkPipelineLayout pipeline_layout;
#define BLEND_MODE_COUNT 4
static VkCommandPool cmd_pool;
static VkCommandBuffer cmd_buffers[16];
static VkBuffer vertex_buffer;
static VkDeviceMemory vertex_memory;
static void *vertex_map;
/* Index buffer: vertices are written once per draw and triangles reference
 * them by index (strips/fans used to be expanded, writing most vertices 3x). */
static VkBuffer index_buffer;
static VkDeviceMemory index_memory;
static void *index_map;
static VkDescriptorSetLayout descriptor_layout;
static VkDescriptorPool descriptor_pool;
static VkSampler texture_sampler;
static VkSampler sampler_linear_repeat;
static VkSampler sampler_linear_clamp;
static VkSampler sampler_nearest_repeat;
static VkSampler sampler_nearest_clamp;
static VkBuffer upload_buffer;
static VkDeviceMemory upload_memory;
static void *upload_map;
static VkFence upload_fence;
/* Texture uploads are recorded into a per-frame-slot command buffer and
 * submitted together with that frame (no per-upload submit + wait, which
 * also waited for every frame in flight -- the big stall when dynamic lights
 * re-upload lightmaps every frame).  Each slot owns UPLOAD_SLOT_BYTES of the
 * staging buffer. */
#define UPLOAD_SLOT_BYTES (8u * 1024u * 1024u)
static VkCommandBuffer upload_cmds[2];
static int upload_recording;
static size_t upload_cursor;
static void flush_uploads_sync(void);
/* Frames in flight: the CPU records frame N+1 while the GPU draws frame N.
 * Each slot owns a command buffer, fence, acquire semaphore and a region of
 * the vertex buffer; the CPU only waits for the slot it is about to reuse. */
#define FRAMES_IN_FLIGHT 2
static VkSemaphore acquire_sems[FRAMES_IN_FLIGHT];
static VkFence frame_fences[FRAMES_IN_FLIGHT];
static VkSemaphore render_sems[16];  /* per swapchain image, waited by present */
static uint32_t cur_frame;
static uint32_t frame_slot_serial[FRAMES_IN_FLIGHT];
static uint32_t frame_serial;        /* serial of the last submitted frame */
static uint32_t completed_serial;    /* every frame <= this has finished */
static int skip_gl_draws = 1;        /* VKSKELETON_GL_PASSTHROUGH=1 disables */
static int im_skip_gl;               /* current glBegin/glEnd not sent to GL */
static uint32_t current_image;
static float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
static int ready;
static int swapchain_dead;
static int swapchain_needs_rebuild;  /* resize: OUT_OF_DATE / SUBOPTIMAL */
static Display *vk_dpy;
static Window vk_win;
static VkSurfaceFormatKHR vk_surface_format;
static int enabled;
static int geometry_enabled;
static int init_attempted;
static int frame_active;
static int frame_has_clear;
static int logged_first_draw;
static int logged_draw_sizes[16];
static int logged_draw_size_count;
static int logged_first_present;
static int logged_unsupported_primitive;
static int log_fd = -1;
static int debug_enabled; /* VKSKELETON_DEBUG=1: extra one-shot logging */

/* VKSKELETON_DEBUG=1: frame counter for dbg_event() log lines. */
static struct {
    unsigned frames;
} dbg;
static void dbg_event(const char *fmt, ...);
static char log_path_buf[512];

enum {
    BLEND_OPAQUE = 0,
    BLEND_ALPHA,
    BLEND_ADD_ALPHA,
    BLEND_ADD_ONE,
};

typedef struct {
    float x, y, z;    /* untransformed GL vertex; the vertex shader applies
                       * the per-batch push-constant matrix */
    float u, v;
    float r, g, b, a;
    float alpha_ref;
    float alpha_func;
    float u2, v2;     /* texture unit 1 (lightmap) coordinates */
    /* Texture slots, blend and depth mode are per-batch state kept in
     * frame_batches[], not per vertex (56 bytes instead of 68). */
} vk_vertex_t;

/* Big maps (bts_rc) plus a previous map's textures exhausted 4096 slots and
 * fell back to the white default.  Slots are now also freed on
 * glDeleteTextures and reused. */
#define MAX_VK_TEXTURES 8192
#define TEXTURE_GL_ID_MAX 65536
static int texture_slot_by_gl[TEXTURE_GL_ID_MAX]; /* stored as slot + 1 */
typedef struct {
    GLuint gl_id;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet desc_set;
    VkSampler sampler;
    uint32_t width, height;
    uint8_t min_filter; /* 0 = linear, 1 = nearest */
    uint8_t mag_filter; /* 0 = linear, 1 = nearest */
    uint8_t wrap_s;     /* 0 = repeat, 1 = clamp */
    uint8_t wrap_t;     /* 0 = repeat, 1 = clamp */
    int valid;
    uint32_t last_used_serial; /* frame serial that last sampled it */
    uint8_t copy_target;       /* debug: receives glCopyTexSubImage2D */
} vk_texture_t;

/* Per-frame vertex budget.  Model-heavy views (crimecity2: ~49k model
 * polys) overflowed the old 262144 limit, and since GoldSrc draws world,
 * then entities, then HUD, the overflow silently dropped models, brush
 * entities and the HUD.  ~68 bytes/vertex: 51 MiB CPU copy + 2 slots of
 * host-visible vertex buffer. */
#define FRAME_MAX_VERTICES 786432
static int logged_frame_overflow;
static void logf_msg(const char *fmt, ...);

static void note_frame_overflow(void)
{
    if (logged_frame_overflow < 4) {
        logged_frame_overflow++;
        logf_msg("vkskeleton: frame vertex buffer full (%u vertices); "
                 "rest of frame dropped\n", (unsigned)FRAME_MAX_VERTICES);
    }
}
#define IMMEDIATE_MAX_VERTICES 65536
#define FRAME_VERTEX_BYTES ((VkDeviceSize)FRAME_MAX_VERTICES * sizeof(vk_vertex_t))
#define FRAME_MAX_INDICES (FRAME_MAX_VERTICES * 2)
#define FRAME_INDEX_BYTES ((VkDeviceSize)FRAME_MAX_INDICES * sizeof(uint32_t))
static uint32_t *frame_indices;   /* this slot's region of index_buffer */
static size_t frame_index_count;
static uint32_t cur_draw_base;    /* first vertex of the current draw */
/* Points into this frame slot's region of the mapped vertex buffer; triangles
 * are written there directly (no CPU staging array + per-frame memcpy). */
static vk_vertex_t *frame_vertices;
static int frame_slot_ready;

typedef struct {
    uint32_t first, count;
    uint32_t tex, tex2;
    uint32_t mat;           /* index into frame_mats[] */
    uint32_t pkey;          /* pipeline state key, see pipeline_key() */
    uint32_t sref;          /* stencil reference (dynamic state) */
    float bias_s, bias_c;   /* depth bias slope/constant (dynamic state) */
} frame_batch_t;
#define MAX_FRAME_MATS 65536
/* Per draw: [0-15] final mvp, [16-19] clip plane (both pushed to the
 * shader), [20-35] GL projection, [36-51] modelview, [52-53] depth-range
 * a,b: the parts vkx eye views rebuild the mvp from. */
#define MAT_FLOATS 56
static float frame_mats_store[FRAMES_IN_FLIGHT][MAX_FRAME_MATS][MAT_FLOATS];
static float (*frame_mats)[MAT_FLOATS] = frame_mats_store[0];
static uint32_t frame_mat_count;
static uint32_t cur_draw_mat;
#define MAX_FRAME_BATCHES 65536
static frame_batch_t frame_batches_store[FRAMES_IN_FLIGHT][MAX_FRAME_BATCHES];
static frame_batch_t *frame_batches = frame_batches_store[0];
static uint32_t frame_batch_count;
static size_t frame_vertex_count;
static vk_texture_t vk_textures[MAX_VK_TEXTURES];
static int free_texture_slots[MAX_VK_TEXTURES];
static int free_texture_slot_count;
static int vk_texture_count = 1; /* slot 0 is the default white texture */

typedef struct {
    float x, y, z;
    float u, v;
    float r, g, b, a;
    float alpha_ref;
    float alpha_func;
    float u2, v2;
    uint32_t tex;
    uint32_t tex2;
    uint8_t blend;
    uint8_t replace;
    uint8_t depth;
} im_vertex_t;

static im_vertex_t im_vertices[IMMEDIATE_MAX_VERTICES];
static size_t im_vertex_count;
static GLenum im_mode;
static int im_active;
static float cur_u, cur_v;
static float cur_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static GLuint cur_texture; /* texture bound on the active unit */
#define MAX_TEX_UNITS 8
#define GL_TEXTURE0_ENUM 0x84c0
#define GL_COMBINE_ENUM 0x8570
#define GL_RGB_SCALE_ENUM 0x8573
static int active_unit;
static GLuint unit_texture[MAX_TEX_UNITS];
static int unit_tex_enabled[MAX_TEX_UNITS];
static GLenum unit_env_mode[MAX_TEX_UNITS] = {
    GL_MODULATE, GL_MODULATE, GL_MODULATE, GL_MODULATE,
    GL_MODULATE, GL_MODULATE, GL_MODULATE, GL_MODULATE};
static float unit_rgb_scale[MAX_TEX_UNITS] = {1, 1, 1, 1, 1, 1, 1, 1};
static float cur_u2, cur_v2;
static GLint unpack_row_length;
static GLint unpack_alignment = 4;
static GLint unpack_skip_rows;
static GLint unpack_skip_pixels;
static int blend_enabled;
static int depth_test_enabled;
static int alpha_test_enabled;
static GLenum cur_alpha_func = GL_ALWAYS;
static GLfloat cur_alpha_ref;
static int vertex_array_enabled;
static int texcoord_array_enabled;
static GLint vertex_array_size = 2;
static GLenum vertex_array_type = GL_FLOAT;
static GLsizei vertex_array_stride;
static const void *vertex_array_ptr;
static GLint texcoord_array_size = 2;
static GLenum texcoord_array_type = GL_FLOAT;
static GLsizei texcoord_array_stride;
static const void *texcoord_array_ptr;
static GLuint current_framebuffer;
static int color_array_enabled;
/* Ordered per-frame command list: draw batches, mid-frame clears and
 * framebuffer->texture copies, recorded in the order the game issued them
 * (mirrors render the mirrored view, copy the screen into a texture with
 * glCopyTexSubImage2D, clear, then render the normal view). */
enum { FCMD_DRAW = 0, FCMD_CLEAR, FCMD_COPY };
typedef struct {
    uint8_t type;
    uint32_t a;          /* DRAW: batch index; COPY: texture slot */
    uint32_t mask;       /* CLEAR: GL clear mask */
    float color[4];      /* CLEAR: clear colour */
    int32_t dx, dy, sx, sy, w, h; /* COPY: GL-space rectangle */
} frame_cmd_t;
#define MAX_FRAME_CMDS 65536
static frame_cmd_t frame_cmds_store[FRAMES_IN_FLIGHT][MAX_FRAME_CMDS];
static frame_cmd_t *frame_cmds = frame_cmds_store[0];
static uint32_t frame_cmd_count;
/* Snapshot of a finished frame for the render side (begin_frame,
 * record_frame_vertices, end_frame_present): taken by the game thread at
 * swap time, consumed synchronously or by the render thread. */
static uint32_t rec_slot;
static frame_cmd_t *rec_cmds;
static uint32_t rec_cmd_count;
static frame_batch_t *rec_batches;
static float (*rec_mats)[MAT_FLOATS];
/* vkx eye views: the finished frame is replayed once per eye, side by side,
 * into eye_image (see vkx_eyes in vkx.h). */
static int rec_eye_count;
static VkExtent2D rec_eye_extent;
static vkx_eye_t rec_eyes[2];
static int rec_hud;               /* module wants the HUD separate */
static int rec_vm_valid;          /* vkx_viewmodel: move the viewmodel */
static float rec_vm[16];
static int cur_eye = -1;          /* replay target: -1 swapchain, else eye */
static VkRect2D cur_rect;         /* replay target area */
static size_t rec_vertex_count;
static int rec_has_upload;
static float rec_clear_color[4];
static float last_proj_tan[4];   /* game side: last perspective projection */
static float rec_proj_tan[4];    /* snapshot for vkx_frame_end */
/* Opt-in render thread (VKSKELETON_RENDER_THREAD=1). */
static int render_thread_enabled;
static pthread_t render_thread;
static int render_thread_started;
static pthread_mutex_t rt_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rt_job_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t rt_done_cv = PTHREAD_COND_INITIALIZER;
static int rt_job_pending, rt_busy;
/* Guards vkQueueSubmit / vkQueuePresentKHR / vkDeviceWaitIdle, which the
 * render thread and the game thread (sync upload flushes) both use. */
static pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER;
static void render_thread_wait_idle(void);
static VkRenderPass render_pass_load; /* LOAD variant to resume after a copy */
static int swap_can_copy;             /* swapchain has TRANSFER_SRC usage */
static int color_mask_off;            /* glColorMask(0,0,0,0): stencil-only */
/* Change counters: draws reuse the previous draw's resolved state / matrix
 * when nothing affecting it changed (the common case across thousands of
 * model strips per frame). */
static unsigned state_serial = 1;     /* texture/blend/depth/stencil/... */
/* While Vulkan presents, tracked GL state calls are not forwarded to OpenGL
 * (it renders nothing then).  gl_out_of_sync records that; gl_sync() pushes
 * all tracked state into GL before anything really draws through it. */
static int gl_out_of_sync;
static GLboolean color_mask_rgba[4] = {1, 1, 1, 1};
static GLuint st_mask = 0xff;
static int gl_skip_state(void);
static void gl_sync(void);
static int should_skip_gl_draw(void);
static unsigned xform_serial = 1;     /* matrices, depth range, clip plane */
static unsigned frame_epoch = 1;      /* bumped whenever frame_mats[] resets */
/* Depth/stencil GL state, folded into the pipeline key of each draw. */
static int stencil_test_enabled;
static GLenum st_func = GL_ALWAYS;
static GLint st_ref;
static GLenum st_sfail = GL_KEEP, st_zfail = GL_KEEP, st_zpass = GL_KEEP;
static int depth_write_enabled = 1;
static GLenum gl_depth_func = GL_LESS;
static int polygon_offset_enabled;  /* GL_POLYGON_OFFSET_FILL */
static int clip0_enabled;           /* GL_CLIP_PLANE0 */
static float clip0_eye[4];          /* plane in eye space (GL semantics) */
static float poly_offset_factor, poly_offset_units;
static float cur_draw_bias_s, cur_draw_bias_c; /* depth bias of the draw */
static uint32_t cur_draw_pkey;  /* pipeline key of the current draw */
static uint32_t cur_draw_sref;  /* stencil reference of the current draw */
static float depth_range_near = 0.0f, depth_range_far = 1.0f;
static GLint color_array_size = 4;
static GLenum color_array_type = GL_FLOAT;
static GLsizei color_array_stride;
static const void *color_array_ptr;
static GLenum cur_blend_src = GL_ONE;
static GLenum cur_blend_dst = GL_ZERO;

typedef struct {
    float m[16];
} mat4;

static mat4 mat_model;
static mat4 mat_proj;
static mat4 mat_texture;
static mat4 mat_color;
static mat4 mat_mvp;
static mat4 stack_model[32];
static mat4 stack_proj[32];
static mat4 stack_texture[32];
static mat4 stack_color[32];
static int sp_model;
static int sp_proj;
static int sp_texture;
static int sp_color;
static GLenum matrix_mode = GL_MODELVIEW;

#define PFN(name) static PFN_vk##name p_vk##name
PFN(CreateInstance);
PFN(EnumeratePhysicalDevices);
PFN(GetPhysicalDeviceQueueFamilyProperties);
PFN(GetPhysicalDeviceMemoryProperties);
PFN(GetPhysicalDeviceFeatures);
PFN(GetPhysicalDeviceSurfaceSupportKHR);
PFN(GetPhysicalDeviceSurfaceCapabilitiesKHR);
PFN(GetPhysicalDeviceSurfaceFormatsKHR);
PFN(GetPhysicalDeviceSurfacePresentModesKHR);
PFN(CreateDevice);
PFN(GetDeviceQueue);
PFN(CreateXlibSurfaceKHR);
PFN(CreateSwapchainKHR);
PFN(GetSwapchainImagesKHR);
PFN(CreateImageView);
PFN(CreateImage);
PFN(GetImageMemoryRequirements);
PFN(BindImageMemory);
PFN(CmdCopyBufferToImage);
PFN(DestroyImage);
PFN(DestroyImageView);
PFN(FreeMemory);
PFN(CreateDescriptorSetLayout);
PFN(CreateDescriptorPool);
PFN(AllocateDescriptorSets);
PFN(UpdateDescriptorSets);
PFN(CreateSampler);
PFN(CreateRenderPass);
PFN(CreateShaderModule);
PFN(CreatePipelineLayout);
PFN(CreateGraphicsPipelines);
PFN(CreateFramebuffer);
PFN(CreateBuffer);
PFN(GetBufferMemoryRequirements);
PFN(AllocateMemory);
PFN(MapMemory);
PFN(UnmapMemory);
PFN(BindBufferMemory);
PFN(CreateCommandPool);
PFN(AllocateCommandBuffers);
PFN(BeginCommandBuffer);
PFN(CmdPipelineBarrier);
PFN(CmdClearColorImage);
PFN(CmdBeginRenderPass);
PFN(CmdEndRenderPass);
PFN(CmdBindPipeline);
PFN(CmdBindDescriptorSets);
PFN(CmdBindVertexBuffers);
PFN(CmdBindIndexBuffer);
PFN(CmdDrawIndexed);
PFN(CmdDraw);
PFN(EndCommandBuffer);
PFN(CreateSemaphore);
PFN(CreateFence);
PFN(ResetFences);
PFN(WaitForFences);
PFN(AcquireNextImageKHR);
PFN(QueueSubmit);
PFN(QueuePresentKHR);
PFN(QueueWaitIdle);
PFN(DestroyFramebuffer);
PFN(DestroySwapchainKHR);
PFN(DeviceWaitIdle);
PFN(CmdSetViewport);
PFN(CmdSetScissor);
PFN(DestroyRenderPass);
PFN(CmdPushConstants);
PFN(CmdBlitImage);
PFN(CmdClearAttachments);
PFN(CmdSetStencilReference);
PFN(CmdSetDepthBias);
#undef PFN

#define LOAD_INSTance(name) \
    p_vk##name = (PFN_vk##name)fp_vkGetInstanceProcAddr(instance, "vk" #name)
#define LOAD_DEVICE(name) \
    p_vk##name = (PFN_vk##name)fp_vkGetDeviceProcAddr(device, "vk" #name)

static void logmsg(const char *s)
{
    size_t n = strlen(s);
    if (log_fd >= 0)
        write(log_fd, s, n);
}

static void logf_msg(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n >= (int)sizeof(buf))
            n = sizeof(buf) - 1;
        if (log_fd >= 0)
            write(log_fd, buf, (size_t)n);
    }
}

/* VKSKELETON_DEBUG=1 only: log each distinct call+arguments once, with the
 * frame number and bound framebuffer (mirror / offscreen investigation). */
static void dbg_event(const char *fmt, ...)
{
    static char seen[600][72];
    static int nseen;
    char buf[72];
    va_list ap;
    int i;

    if (!debug_enabled)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (i = 0; i < nseen; i++)
        if (!strcmp(seen[i], buf))
            return;
    if (nseen >= 600)
        return;
    memcpy(seen[nseen++], buf, sizeof(buf));
    logf_msg("vkskeleton: dbg mirror f=%u fb=%u %s\n", dbg.frames,
             (unsigned)current_framebuffer, buf);
}

static void *next_symbol(const char *name)
{
    return real_dlsym ? real_dlsym(RTLD_NEXT, name) : NULL;
}

/* ---------- small column-major matrix subset ---------- */

static void mat_identity(mat4 *o)
{
    memset(o, 0, sizeof(*o));
    o->m[0] = o->m[5] = o->m[10] = o->m[15] = 1.0f;
}

static void mat_mul(mat4 *o, const mat4 *a, const mat4 *b)
{
    float r[16];
    int c, row, k;

    for (c = 0; c < 4; c++) {
        for (row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (k = 0; k < 4; k++)
                sum += a->m[k * 4 + row] * b->m[c * 4 + k];
            r[c * 4 + row] = sum;
        }
    }
    memcpy(o->m, r, sizeof(r));
}

static mat4 *current_matrix(void)
{
    switch (matrix_mode) {
    case GL_PROJECTION:
        return &mat_proj;
    case GL_TEXTURE:
        return &mat_texture;
    case GL_COLOR:
        return &mat_color;
    default:
        return &mat_model;
    }
}

static mat4 *current_stack(void)
{
    switch (matrix_mode) {
    case GL_PROJECTION:
        return stack_proj;
    case GL_TEXTURE:
        return stack_texture;
    case GL_COLOR:
        return stack_color;
    default:
        return stack_model;
    }
}

static int *current_stack_ptr(void)
{
    switch (matrix_mode) {
    case GL_PROJECTION:
        return &sp_proj;
    case GL_TEXTURE:
        return &sp_texture;
    case GL_COLOR:
        return &sp_color;
    default:
        return &sp_model;
    }
}

/* General 4x4 inverse (column-major, cofactor expansion). Returns 0 if
 * singular. */
static int mat_invert(const float *m, float *out)
{
    float inv[16], det;
    int i;

    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
             m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
             m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
             m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
              m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
             m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
             m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
             m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
              m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
             m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
             m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
              m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
              m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
             m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
             m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
              m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
              m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f)
        return 0;
    det = 1.0f / det;
    for (i = 0; i < 16; i++)
        out[i] = inv[i] * det;
    return 1;
}

static void mat_translate(mat4 *m, float x, float y, float z)
{
    mat4 t;
    mat_identity(&t);
    t.m[12] = x;
    t.m[13] = y;
    t.m[14] = z;
    mat_mul(m, m, &t);
}

static void mat_scale(mat4 *m, float x, float y, float z)
{
    mat4 t;
    mat_identity(&t);
    t.m[0] = x;
    t.m[5] = y;
    t.m[10] = z;
    mat_mul(m, m, &t);
}

static void mat_rotate(mat4 *m, float angle, float x, float y, float z)
{
    float len = sqrtf(x * x + y * y + z * z);
    float c, s, t, rad;
    mat4 r;

    if (len <= 0.0f)
        return;
    x /= len;
    y /= len;
    z /= len;
    rad = angle * (float)M_PI / 180.0f;
    c = cosf(rad);
    s = sinf(rad);
    t = 1.0f - c;

    memset(&r, 0, sizeof(r));
    r.m[0] = t * x * x + c;
    r.m[1] = t * x * y + s * z;
    r.m[2] = t * x * z - s * y;
    r.m[4] = t * x * y - s * z;
    r.m[5] = t * y * y + c;
    r.m[6] = t * y * z + s * x;
    r.m[8] = t * x * z + s * y;
    r.m[9] = t * y * z - s * x;
    r.m[10] = t * z * z + c;
    r.m[15] = 1.0f;
    mat_mul(m, m, &r);
}

static void mat_ortho(mat4 *m, GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                      GLdouble n, GLdouble f)
{
    mat4 o;

    memset(&o, 0, sizeof(o));
    o.m[0] = (float)(2.0 / (r - l));
    o.m[5] = (float)(2.0 / (t - b));
    o.m[10] = (float)(-2.0 / (f - n));
    o.m[12] = (float)(-(r + l) / (r - l));
    o.m[13] = (float)(-(t + b) / (t - b));
    o.m[14] = (float)(-(f + n) / (f - n));
    o.m[15] = 1.0f;
    mat_mul(m, m, &o);
}

static void mat_frustum(mat4 *m, GLdouble l, GLdouble r, GLdouble b,
                        GLdouble t, GLdouble n, GLdouble f)
{
    mat4 o;

    memset(&o, 0, sizeof(o));
    o.m[0] = (float)(2.0 * n / (r - l));
    o.m[5] = (float)(2.0 * n / (t - b));
    o.m[8] = (float)((r + l) / (r - l));
    o.m[9] = (float)((t + b) / (t - b));
    o.m[10] = (float)(-(f + n) / (f - n));
    o.m[11] = -1.0f;
    o.m[14] = (float)(-2.0 * f * n / (f - n));
    mat_mul(m, m, &o);
}

/* ---------- X11 window capture ---------- */

static void track_window(Display *dpy, Window window, Window parent,
                         unsigned int width, unsigned int height)
{
    int i;

    if (!enabled || !dpy || !window)
        return;

    for (i = 0; i < tracked_count; i++) {
        if (tracked[i].dpy == dpy && tracked[i].window == window) {
            tracked[i].width = width;
            tracked[i].height = height;
            return;
        }
    }
    if (tracked_count >= MAX_TRACKED_WINDOWS)
        return;

    i = tracked_count++;
    tracked[i].dpy = dpy;
    tracked[i].window = window;
    tracked[i].parent = parent;
    tracked[i].width = width;
    tracked[i].height = height;
    tracked[i].mapped = 0;

    logf_msg("vkskeleton: captured X window %lu parent=%lu %ux%u\n",
             (unsigned long)window, (unsigned long)parent, width, height);
}

static void mark_mapped(Display *dpy, Window window)
{
    int i;

    if (!enabled)
        return;
    for (i = 0; i < tracked_count; i++) {
        if (tracked[i].dpy == dpy && tracked[i].window == window) {
            if (!tracked[i].mapped)
                logf_msg("vkskeleton: X window %lu mapped\n",
                         (unsigned long)window);
            tracked[i].mapped = 1;
            return;
        }
    }
}

static int choose_window(Display **dpy_out, Window *win_out)
{
    int best = -1;
    unsigned long best_score = 0;
    int i;

    for (i = 0; i < tracked_count; i++) {
        unsigned long area;
        unsigned long score;

        if (tracked[i].width < 64 || tracked[i].height < 64)
            continue;
        area = (unsigned long)tracked[i].width *
               (unsigned long)tracked[i].height;
        score = area + (tracked[i].mapped ? 0x10000000UL : 0);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    if (best < 0) {
        logmsg("vkskeleton: no usable captured X window\n");
        return 0;
    }

    logf_msg("vkskeleton: selected X window %lu %ux%u mapped=%d\n",
             (unsigned long)tracked[best].window, tracked[best].width,
             tracked[best].height, tracked[best].mapped);
    *dpy_out = tracked[best].dpy;
    *win_out = tracked[best].window;
    return 1;
}

Window XCreateWindow(Display *dpy, Window parent, int x, int y,
                     unsigned int width, unsigned int height,
                     unsigned int border_width, int depth, unsigned int klass,
                     Visual *visual, unsigned long valuemask,
                     XSetWindowAttributes *attributes)
{
    Window result;

    if (!real_x_create_window)
        real_x_create_window = (fn_x_create_window)next_symbol("XCreateWindow");
    result = real_x_create_window
                 ? real_x_create_window(dpy, parent, x, y, width, height,
                                        border_width, depth, klass, visual,
                                        valuemask, attributes)
                 : None;
    track_window(dpy, result, parent, width, height);
    return result;
}

Window XCreateSimpleWindow(Display *dpy, Window parent, int x, int y,
                           unsigned int width, unsigned int height,
                           unsigned int border_width, unsigned long border,
                           unsigned long background)
{
    Window result;

    if (!real_x_create_simple_window)
        real_x_create_simple_window =
            (fn_x_create_simple_window)next_symbol("XCreateSimpleWindow");
    result = real_x_create_simple_window
                 ? real_x_create_simple_window(dpy, parent, x, y, width,
                                               height, border_width, border,
                                               background)
                 : None;
    track_window(dpy, result, parent, width, height);
    return result;
}

int XMapWindow(Display *dpy, Window window)
{
    int result;

    if (!real_x_map_window)
        real_x_map_window = (fn_x_map_window)next_symbol("XMapWindow");
    result = real_x_map_window ? real_x_map_window(dpy, window) : 0;
    if (result)
        mark_mapped(dpy, window);
    return result;
}

int XMapRaised(Display *dpy, Window window)
{
    int result;

    if (!real_x_map_raised)
        real_x_map_raised = (fn_x_map_raised)next_symbol("XMapRaised");
    result = real_x_map_raised ? real_x_map_raised(dpy, window) : 0;
    if (result)
        mark_mapped(dpy, window);
    return result;
}

/* ---------- Vulkan setup ---------- */

static uint32_t find_memory_type(VkMemoryRequirements req,
                                 VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties props;
    uint32_t i;

    memset(&props, 0, sizeof(props));
    p_vkGetPhysicalDeviceMemoryProperties(physdev, &props);
    for (i = 0; i < props.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    }
    return ~0u;
}

/* Wait until no submitted frame can still be reading this texture. Needed
 * before destroying it or rewriting its descriptor set (both are illegal
 * while a pending command buffer uses them). */
static void wait_texture_idle(const vk_texture_t *tex)
{
    if (render_thread_enabled)
        render_thread_wait_idle();
    if (!device || !tex || tex->last_used_serial <= completed_serial)
        return;
    p_vkWaitForFences(device, FRAMES_IN_FLIGHT, frame_fences, VK_TRUE,
                      UINT64_MAX);
    completed_serial = frame_serial;
}

static void destroy_texture_image(vk_texture_t *tex)
{
    if (render_thread_enabled)
        render_thread_wait_idle();
    state_serial++;
    if (tex->image && upload_recording)
        flush_uploads_sync();
    wait_texture_idle(tex);
    if (tex->view) {
        p_vkDestroyImageView(device, tex->view, NULL);
        tex->view = VK_NULL_HANDLE;
    }
    if (tex->image) {
        p_vkDestroyImage(device, tex->image, NULL);
        tex->image = VK_NULL_HANDLE;
    }
    if (tex->memory) {
        p_vkFreeMemory(device, tex->memory, NULL);
        tex->memory = VK_NULL_HANDLE;
    }
    tex->valid = 0;
}

static vk_texture_t *get_texture_slot(GLuint gl_id, int create)
{
    int i;

    if (gl_id == 0)
        return &vk_textures[0];
    if (gl_id < TEXTURE_GL_ID_MAX && texture_slot_by_gl[gl_id])
        return &vk_textures[texture_slot_by_gl[gl_id] - 1];
    for (i = 1; i < vk_texture_count; i++) {
        if (vk_textures[i].gl_id == gl_id) {
            if (gl_id < TEXTURE_GL_ID_MAX)
                texture_slot_by_gl[gl_id] = i + 1;
            return &vk_textures[i];
        }
    }
    if (create && free_texture_slot_count > 0) {
        VkDescriptorSet keep;
        i = free_texture_slots[--free_texture_slot_count];
        keep = vk_textures[i].desc_set; /* pool sets are never freed */
        memset(&vk_textures[i], 0, sizeof(vk_textures[i]));
        vk_textures[i].desc_set = keep;
        vk_textures[i].gl_id = gl_id;
        if (gl_id < TEXTURE_GL_ID_MAX)
            texture_slot_by_gl[gl_id] = i + 1;
        return &vk_textures[i];
    }
    if (!create || vk_texture_count >= MAX_VK_TEXTURES) {
        if (create) {
            static int logged_miss;
            if (logged_miss < 8) {
                logged_miss++;
                logf_msg("vkskeleton: texture slot exhausted GL=%u\n",
                         (unsigned)gl_id);
            }
        }
        return NULL;
    }

    i = vk_texture_count++;
    memset(&vk_textures[i], 0, sizeof(vk_textures[i]));
    vk_textures[i].gl_id = gl_id;
    if (gl_id < TEXTURE_GL_ID_MAX)
        texture_slot_by_gl[gl_id] = i + 1;
    return &vk_textures[i];
}

static VkSampler sampler_for_texture(const vk_texture_t *tex)
{
    if (!tex)
        return texture_sampler;
    if (tex->min_filter || tex->mag_filter) {
        if (tex->min_filter && tex->mag_filter) {
            return tex->wrap_s || tex->wrap_t ? sampler_nearest_clamp
                                              : sampler_nearest_repeat;
        }
    } else if (tex->wrap_s || tex->wrap_t) {
        return sampler_linear_clamp;
    }
    if (tex->min_filter || tex->mag_filter)
        return tex->wrap_s || tex->wrap_t ? sampler_nearest_clamp
                                          : sampler_nearest_repeat;
    return texture_sampler;
}

static void refresh_texture_descriptor(vk_texture_t *tex)
{
    if (render_thread_enabled && tex && tex->desc_set)
        render_thread_wait_idle();
    VkDescriptorImageInfo image_info = {0};
    VkWriteDescriptorSet write = {0};

    if (!tex || !tex->desc_set || !tex->view)
        return;
    wait_texture_idle(tex);
    tex->sampler = sampler_for_texture(tex);
    image_info.sampler = tex->sampler ? tex->sampler : texture_sampler;
    image_info.imageView = tex->view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = tex->desc_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    p_vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
}

static int blend_mode_from_state(void)
{
    if (!blend_enabled || cur_blend_src == GL_ONE || cur_blend_dst == GL_ZERO) {
        if (cur_blend_src == GL_ONE && cur_blend_dst == GL_ZERO)
            return BLEND_OPAQUE;
        if (!blend_enabled)
            return BLEND_OPAQUE;
    }
    if (cur_blend_src == GL_SRC_ALPHA && cur_blend_dst == GL_ONE_MINUS_SRC_ALPHA)
        return BLEND_ALPHA;
    if (cur_blend_src == GL_SRC_ALPHA && cur_blend_dst == GL_ONE)
        return BLEND_ADD_ALPHA;
    if (cur_blend_src == GL_ONE && cur_blend_dst == GL_ONE)
        return BLEND_ADD_ONE;
    return BLEND_OPAQUE;
}

static void alpha_params_for_state(float *ref, float *func)
{
    if (!alpha_test_enabled || cur_alpha_func == GL_ALWAYS) {
        *ref = -1.0f;
        *func = 0.0f;
        return;
    }
    *ref = cur_alpha_ref;
    *func = (float)cur_alpha_func;
}


static void transition_upload_image(VkImage image, VkImageLayout old_layout,
                                    VkImageLayout new_layout,
                                    VkAccessFlags src_access,
                                    VkAccessFlags dst_access,
                                    VkPipelineStageFlags src_stage,
                                    VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {0};

    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    p_vkCmdPipelineBarrier(upload_cmds[cur_frame], src_stage, dst_stage, 0, 0, NULL, 0,
                           NULL, 1, &barrier);
}

/* Start (or continue) recording this slot's upload batch. */
static int upload_batch_begin(void)
{
    VkCommandBufferBeginInfo begin = {0};

    if (upload_recording)
        return 1;
    /* The slot's command buffer and staging region were last used by the
     * frame submitted FRAMES_IN_FLIGHT frames ago. */
    p_vkWaitForFences(device, 1, &frame_fences[cur_frame], VK_TRUE, UINT64_MAX);
    p_vkWaitForFences(device, 1, &upload_fence, VK_TRUE, UINT64_MAX);
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (p_vkBeginCommandBuffer(upload_cmds[cur_frame], &begin) != VK_SUCCESS)
        return 0;
    upload_recording = 1;
    upload_cursor = 0;
    return 1;
}

/* Submit the pending batch on its own and wait (staging region full,
 * a texture with pending copies is destroyed, or the frame is not
 * presented by Vulkan).  Rare during play. */
static void flush_uploads_sync(void)
{
    VkSubmitInfo si = {0};
    VkResult r;

    if (!upload_recording)
        return;
    upload_recording = 0;
    p_vkEndCommandBuffer(upload_cmds[cur_frame]);
    p_vkResetFences(device, 1, &upload_fence);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &upload_cmds[cur_frame];
    pthread_mutex_lock(&queue_mtx);
    r = p_vkQueueSubmit(queue, 1, &si, upload_fence);
    pthread_mutex_unlock(&queue_mtx);
    if (r != VK_SUCCESS) {
        logmsg("vkskeleton: upload flush submit failed\n");
        return;
    }
    p_vkWaitForFences(device, 1, &upload_fence, VK_TRUE, UINT64_MAX);
    upload_cursor = 0;
}

static void upload_texture_pixels(vk_texture_t *tex, uint32_t xoff,
                                  uint32_t yoff, uint32_t w, uint32_t h,
                                  const unsigned char *pixels, int src_bpp)
{
    state_serial++;
    VkBufferImageCopy copy = {0};
    VkImageLayout old_layout;
    const unsigned char *src;
    unsigned char *staging;
    size_t need;
    size_t row_bytes;
    size_t src_row_pixels;
    size_t src_stride;
    size_t align;
    uint32_t row;

    if (!device || !upload_map || !upload_cmds[0] || !upload_fence || !tex->image)
        return;
    if (!w || !h || xoff + w > tex->width || yoff + h > tex->height)
        return;
    row_bytes = (size_t)w * 4;
    src_row_pixels = unpack_row_length > (GLint)w
                         ? (size_t)unpack_row_length
                         : (size_t)w;
    src_stride = src_row_pixels * (size_t)src_bpp;
    align = unpack_alignment > 0 ? (size_t)unpack_alignment : 4;
    src_stride = (src_stride + align - 1) / align * align;
    if ((size_t)tex->width * tex->height * 4 > UPLOAD_SLOT_BYTES)
        return;

    need = pixels ? ((row_bytes * h + 15) & ~(size_t)15) : 0;
    if (upload_recording && upload_cursor + need > UPLOAD_SLOT_BYTES)
        flush_uploads_sync();
    if (!upload_batch_begin())
        return;
    staging = (unsigned char *)upload_map +
              (size_t)cur_frame * UPLOAD_SLOT_BYTES + upload_cursor;

    if (pixels) {
        src = pixels + (size_t)unpack_skip_rows * src_stride +
              (size_t)unpack_skip_pixels * (size_t)src_bpp;
        /* GL's first row is the bottom row (v=0), and Vulkan image row 0 is
         * also sampled at v=0.  Keep the row order unchanged; the matching
         * Y flip is applied to vertex positions in transform_vertex().
         *
         * Pack the sub-image tightly at column 0 of the staging buffer:
         * VkBufferImageCopy always reads source rows starting at the first
         * texel, and imageOffset only shifts the destination.  Copying at
         * xoff here would make every sub-image read stale pixels from the
         * start of the staging buffer (this corrupted the font atlas, whose
         * glyphs are uploaded as individual sub-rectangles). */
        for (row = 0; row < h; row++) {
            unsigned char *dst = staging + (size_t)row * row_bytes;
            const unsigned char *s = src + (size_t)row * src_stride;
            if (src_bpp == 4) {
                memcpy(dst, s, row_bytes);
            } else { /* GL_RGB: expand to RGBA with opaque alpha */
                uint32_t x;
                for (x = 0; x < w; x++) {
                    dst[x * 4 + 0] = s[x * 3 + 0];
                    dst[x * 4 + 1] = s[x * 3 + 1];
                    dst[x * 4 + 2] = s[x * 3 + 2];
                    dst[x * 4 + 3] = 255;
                }
            }
        }
    }

    old_layout = tex->valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED;
    if (pixels) {
        transition_upload_image(tex->image, old_layout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                tex->valid ? VK_ACCESS_SHADER_READ_BIT : 0,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                tex->valid ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                           : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    copy.bufferOffset = (VkDeviceSize)cur_frame * UPLOAD_SLOT_BYTES + upload_cursor;
    copy.bufferRowLength = (uint32_t)w;
    copy.imageOffset.x = (int32_t)xoff;
    copy.imageOffset.y = (int32_t)yoff;
    copy.imageExtent.width = w;
    copy.imageExtent.height = h;
    copy.imageExtent.depth = 1;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    if (pixels) {
        p_vkCmdCopyBufferToImage(upload_cmds[cur_frame], upload_buffer, tex->image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &copy);
        transition_upload_image(tex->image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    } else if (!tex->valid) {
        transition_upload_image(tex->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                                VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    upload_cursor += need;
    tex->valid = 1;
}

static int create_vk_texture_image(vk_texture_t *tex, uint32_t width,
                                   uint32_t height, const void *pixels,
                                   int src_bpp)
{
    state_serial++;
    VkImageCreateInfo image_ci = {0};
    VkMemoryRequirements mem_req;
    VkMemoryAllocateInfo alloc = {0};
    VkImageViewCreateInfo view_ci = {0};
    VkDescriptorSetAllocateInfo dsai = {0};
    VkResult r;

    if (!width || !height || width > 2048 || height > 2048)
        return 0;

    destroy_texture_image(tex);

    image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_ci.extent.width = width;
    image_ci.extent.height = height;
    image_ci.extent.depth = 1;
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = p_vkCreateImage(device, &image_ci, NULL, &tex->image);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: CreateImage failed (%d)\n", (int)r);
        return 0;
    }

    p_vkGetImageMemoryRequirements(device, tex->image, &mem_req);
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = mem_req.size;
    alloc.memoryTypeIndex =
        find_memory_type(mem_req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == ~0u) {
        logmsg("vkskeleton: no device-local image memory\n");
        destroy_texture_image(tex);
        return 0;
    }
    r = p_vkAllocateMemory(device, &alloc, NULL, &tex->memory);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: image AllocateMemory failed (%d)\n", (int)r);
        destroy_texture_image(tex);
        return 0;
    }
    r = p_vkBindImageMemory(device, tex->image, tex->memory, 0);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: BindImageMemory failed (%d)\n", (int)r);
        destroy_texture_image(tex);
        return 0;
    }

    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = tex->image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    r = p_vkCreateImageView(device, &view_ci, NULL, &tex->view);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: texture CreateImageView failed (%d)\n", (int)r);
        destroy_texture_image(tex);
        return 0;
    }

    if (!tex->desc_set) {
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descriptor_pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &descriptor_layout;
        r = p_vkAllocateDescriptorSets(device, &dsai, &tex->desc_set);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: AllocateDescriptorSets failed (%d)\n", (int)r);
            destroy_texture_image(tex);
            return 0;
        }
    }

    refresh_texture_descriptor(tex);

    tex->width = width;
    tex->height = height;
    upload_texture_pixels(tex, 0, 0, width, height, pixels, src_bpp);
    return 1;
}

/* Image views, depth buffer and framebuffers: everything sized to the
 * swapchain.  Rebuilt whenever the window is resized. */
static int create_swap_resources(void)
{
    VkMemoryRequirements mem_req;
    VkResult r;
    uint32_t i;

    for (i = 0; i < swap_image_count; i++) {
        VkImageViewCreateInfo ivci = {0};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = swap_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = swap_format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        r = p_vkCreateImageView(device, &ivci, NULL, &image_views[i]);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: CreateImageView %u failed (%d)\n", i, (int)r);
            return 0;
        }
    }
    logmsg("vkskeleton: image views ready\n");

    {
        VkImageCreateInfo depth_ci = {0};
        VkImageViewCreateInfo depth_vi = {0};
        VkMemoryAllocateInfo depth_ai = {0};

        depth_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depth_ci.imageType = VK_IMAGE_TYPE_2D;
        depth_ci.format = VK_DEPTH_FORMAT;
        depth_ci.extent.width = swap_extent.width;
        depth_ci.extent.height = swap_extent.height;
        depth_ci.extent.depth = 1;
        depth_ci.mipLevels = 1;
        depth_ci.arrayLayers = 1;
        depth_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        depth_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depth_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        depth_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        r = p_vkCreateImage(device, &depth_ci, NULL, &depth_image);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: depth image failed (%d)\n", (int)r);
            return 0;
        }
        p_vkGetImageMemoryRequirements(device, depth_image, &mem_req);
        depth_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        depth_ai.allocationSize = mem_req.size;
        depth_ai.memoryTypeIndex =
            find_memory_type(mem_req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (depth_ai.memoryTypeIndex == ~0u ||
            p_vkAllocateMemory(device, &depth_ai, NULL, &depth_memory) !=
                VK_SUCCESS ||
            p_vkBindImageMemory(device, depth_image, depth_memory, 0) !=
                VK_SUCCESS) {
            logmsg("vkskeleton: depth memory failed\n");
            return 0;
        }
        depth_vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depth_vi.image = depth_image;
        depth_vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depth_vi.format = VK_DEPTH_FORMAT;
        depth_vi.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        depth_vi.subresourceRange.levelCount = 1;
        depth_vi.subresourceRange.layerCount = 1;
        r = p_vkCreateImageView(device, &depth_vi, NULL, &depth_view);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: depth view failed (%d)\n", (int)r);
            return 0;
        }
        logmsg("vkskeleton: depth image ready\n");
    }

    for (i = 0; i < swap_image_count; i++) {
        VkFramebufferCreateInfo fci = {0};
        VkImageView framebuffer_attachments[2] = {image_views[i], depth_view};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = render_pass;
        fci.attachmentCount = 2;
        fci.pAttachments = framebuffer_attachments;
        fci.width = swap_extent.width;
        fci.height = swap_extent.height;
        fci.layers = 1;
        r = p_vkCreateFramebuffer(device, &fci, NULL, &framebuffers[i]);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: CreateFramebuffer %u failed (%d)\n", i,
                     (int)r);
            return 0;
        }
    }
    logmsg("vkskeleton: framebuffers ready\n");
    return 1;
}

static void destroy_swap_resources(void)
{
    uint32_t i;

    for (i = 0; i < 16; i++) {
        if (framebuffers[i]) {
            p_vkDestroyFramebuffer(device, framebuffers[i], NULL);
            framebuffers[i] = VK_NULL_HANDLE;
        }
        if (image_views[i]) {
            p_vkDestroyImageView(device, image_views[i], NULL);
            image_views[i] = VK_NULL_HANDLE;
        }
    }
    if (depth_view) {
        p_vkDestroyImageView(device, depth_view, NULL);
        depth_view = VK_NULL_HANDLE;
    }
    if (depth_image) {
        p_vkDestroyImage(device, depth_image, NULL);
        depth_image = VK_NULL_HANDLE;
    }
    if (depth_memory) {
        p_vkFreeMemory(device, depth_memory, NULL);
        depth_memory = VK_NULL_HANDLE;
    }
}

static int create_graphics_resources(void)
{
    VkAttachmentDescription color_att = {0};
    VkAttachmentDescription depth_att = {0};
    VkAttachmentDescription attachments[2];
    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {0};
    VkRenderPassCreateInfo rpci = {0};
    VkShaderModuleCreateInfo smci = {0};
    VkPipelineLayoutCreateInfo plci = {0};
    VkBufferCreateInfo bci = {0};
    VkMemoryRequirements mem_req;
    VkMemoryAllocateInfo mai = {0};
    VkResult r;
    uint32_t i;

    color_att.format = swap_format;
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    depth_att.format = VK_DEPTH_FORMAT;
    depth_att.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    /* stored: a mid-frame screen copy splits the frame into two passes */
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[0] = color_att;
    attachments[1] = depth_att;

    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = attachments;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    r = p_vkCreateRenderPass(device, &rpci, NULL, &render_pass);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: CreateRenderPass failed (%d)\n", (int)r);
        return 0;
    }
    {
        /* Same attachments (so the framebuffers stay compatible) but LOAD
         * everything: resumes drawing after a framebuffer->texture copy. */
        VkAttachmentDescription load_atts[2];
        load_atts[0] = color_att;
        load_atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        load_atts[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        load_atts[1] = depth_att;
        load_atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        load_atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        load_atts[1].initialLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        rpci.pAttachments = load_atts;
        r = p_vkCreateRenderPass(device, &rpci, NULL, &render_pass_load);
        rpci.pAttachments = attachments;
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: CreateRenderPass(load) failed (%d)\n", (int)r);
            return 0;
        }
    }
    logmsg("vkskeleton: render pass ready\n");

    if (!create_swap_resources())
        return 0;

    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = VK_VERTEX_SPIRV_BYTES;
    smci.pCode = vk_vertex_spirv;
    r = p_vkCreateShaderModule(device, &smci, NULL, &vert_module);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: vertex shader module failed (%d)\n", (int)r);
        return 0;
    }
    logmsg("vkskeleton: vertex shader ready\n");
    smci.codeSize = VK_FRAGMENT_SPIRV_BYTES;
    smci.pCode = vk_fragment_spirv;
    r = p_vkCreateShaderModule(device, &smci, NULL, &frag_module);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: fragment shader module failed (%d)\n", (int)r);
        return 0;
    }
    logmsg("vkskeleton: fragment shader ready\n");

    {
        VkDescriptorSetLayoutBinding ds_binding = {0};
        VkDescriptorSetLayoutCreateInfo dslci = {0};
        VkDescriptorPoolSize pool_size = {0};
        VkDescriptorPoolCreateInfo dpci = {0};
        VkSamplerCreateInfo sampler_ci = {0};
        VkBufferCreateInfo upload_bci = {0};
        VkMemoryAllocateInfo upload_mai = {0};
        VkCommandBufferAllocateInfo upload_cbai = {0};
        VkFenceCreateInfo upload_fi = {0};
        static const unsigned char white_pixel[4] = {255, 255, 255, 255};

        ds_binding.binding = 0;
        ds_binding.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ds_binding.descriptorCount = 1;
        ds_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 1;
        dslci.pBindings = &ds_binding;
        r = p_vkCreateDescriptorSetLayout(device, &dslci, NULL,
                                          &descriptor_layout);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: descriptor layout failed (%d)\n", (int)r);
            return 0;
        }

        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = MAX_VK_TEXTURES;
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = MAX_VK_TEXTURES;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &pool_size;
        r = p_vkCreateDescriptorPool(device, &dpci, NULL, &descriptor_pool);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: descriptor pool failed (%d)\n", (int)r);
            return 0;
        }

        {
            VkSampler *samplers[4] = {
                &sampler_linear_repeat, &sampler_linear_clamp,
                &sampler_nearest_repeat, &sampler_nearest_clamp,
            };
            VkFilter sampler_filters[4] = {
                VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                VK_FILTER_NEAREST, VK_FILTER_NEAREST,
            };
            VkSamplerAddressMode sampler_modes[4] = {
                VK_SAMPLER_ADDRESS_MODE_REPEAT,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_REPEAT,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            };

            for (i = 0; i < 4; i++) {
                sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                sampler_ci.magFilter = sampler_filters[i];
                sampler_ci.minFilter = sampler_filters[i];
                sampler_ci.addressModeU = sampler_modes[i];
                sampler_ci.addressModeV = sampler_modes[i];
                sampler_ci.addressModeW = sampler_modes[i];
                sampler_ci.unnormalizedCoordinates = VK_FALSE;
                sampler_ci.compareEnable = VK_FALSE;
                r = p_vkCreateSampler(device, &sampler_ci, NULL, samplers[i]);
                if (r != VK_SUCCESS) {
                    logf_msg("vkskeleton: sampler %u failed (%d)\n", i, (int)r);
                    return 0;
                }
            }
            texture_sampler = sampler_linear_repeat;
        }

        upload_bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        upload_bci.size = (VkDeviceSize)UPLOAD_SLOT_BYTES * FRAMES_IN_FLIGHT;
        upload_bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        upload_bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        r = p_vkCreateBuffer(device, &upload_bci, NULL, &upload_buffer);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: upload buffer failed (%d)\n", (int)r);
            return 0;
        }
        p_vkGetBufferMemoryRequirements(device, upload_buffer, &mem_req);
        upload_mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        upload_mai.allocationSize = mem_req.size;
        upload_mai.memoryTypeIndex =
            find_memory_type(mem_req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (upload_mai.memoryTypeIndex == ~0u ||
            p_vkAllocateMemory(device, &upload_mai, NULL, &upload_memory) !=
                VK_SUCCESS ||
            p_vkBindBufferMemory(device, upload_buffer, upload_memory, 0) !=
                VK_SUCCESS ||
            p_vkMapMemory(device, upload_memory, 0, mem_req.size, 0,
                          &upload_map) != VK_SUCCESS) {
            logmsg("vkskeleton: upload memory failed\n");
            return 0;
        }

        upload_cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        upload_cbai.commandPool = cmd_pool;
        upload_cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        upload_cbai.commandBufferCount = FRAMES_IN_FLIGHT;
        r = p_vkAllocateCommandBuffers(device, &upload_cbai, upload_cmds);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: upload command buffer failed (%d)\n", (int)r);
            return 0;
        }
        upload_fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        upload_fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        r = p_vkCreateFence(device, &upload_fi, NULL, &upload_fence);
        if (r != VK_SUCCESS) {
            logf_msg("vkskeleton: upload fence failed (%d)\n", (int)r);
            return 0;
        }
        logmsg("vkskeleton: texture resources ready\n");

        if (!create_vk_texture_image(&vk_textures[0], 1, 1, white_pixel, 4)) {
            logmsg("vkskeleton: default white texture failed\n");
            return 0;
        }
        logmsg("vkskeleton: default texture ready\n");
    }

    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    {
        static VkDescriptorSetLayout set_layouts[2];
        set_layouts[0] = descriptor_layout;
        set_layouts[1] = descriptor_layout;
        plci.setLayoutCount = 2;
        plci.pSetLayouts = set_layouts;
        {
            /* mat4 mvp for the vertex shader (one per batch). */
            static VkPushConstantRange pcr;
            pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            pcr.offset = 0;
            pcr.size = 80; /* mat4 mvp + vec4 clip plane */
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &pcr;
        }
    }
    r = p_vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: CreatePipelineLayout failed (%d)\n", (int)r);
        return 0;
    }
    logmsg("vkskeleton: pipeline layout ready\n");

    /* graphics pipelines are created on first use: get_pipeline() */

    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = FRAME_VERTEX_BYTES * FRAMES_IN_FLIGHT;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = p_vkCreateBuffer(device, &bci, NULL, &vertex_buffer);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: CreateBuffer failed (%d)\n", (int)r);
        return 0;
    }
    p_vkGetBufferMemoryRequirements(device, vertex_buffer, &mem_req);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mem_req.size;
    mai.memoryTypeIndex =
        find_memory_type(mem_req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == ~0u) {
        logmsg("vkskeleton: no host-visible memory type\n");
        return 0;
    }
    r = p_vkAllocateMemory(device, &mai, NULL, &vertex_memory);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: AllocateMemory failed (%d)\n", (int)r);
        return 0;
    }
    r = p_vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: BindBufferMemory failed (%d)\n", (int)r);
        return 0;
    }
    r = p_vkMapMemory(device, vertex_memory, 0, mem_req.size, 0, &vertex_map);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: MapMemory failed (%d)\n", (int)r);
        return 0;
    }
    logmsg("vkskeleton: vertex buffer ready\n");

    bci.size = FRAME_INDEX_BYTES * FRAMES_IN_FLIGHT;
    bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    r = p_vkCreateBuffer(device, &bci, NULL, &index_buffer);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: index CreateBuffer failed (%d)\n", (int)r);
        return 0;
    }
    p_vkGetBufferMemoryRequirements(device, index_buffer, &mem_req);
    mai.allocationSize = mem_req.size;
    mai.memoryTypeIndex =
        find_memory_type(mem_req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == ~0u ||
        p_vkAllocateMemory(device, &mai, NULL, &index_memory) != VK_SUCCESS ||
        p_vkBindBufferMemory(device, index_buffer, index_memory, 0) !=
            VK_SUCCESS ||
        p_vkMapMemory(device, index_memory, 0, mem_req.size, 0, &index_map) !=
            VK_SUCCESS) {
        logmsg("vkskeleton: index buffer memory failed\n");
        return 0;
    }
    logmsg("vkskeleton: index buffer ready\n");

    logmsg("vkskeleton: graphics resources ready\n");
    return 1;
}

/* (Re)create the swapchain at the window's current size.  Returns 1 on
 * success, 0 on failure, -1 if the window has no area (minimized). */
/* Present mode.  Under Xwayland (and X11 generally) Mesa presents
 * IMMEDIATE swapchains on the calling thread and blocks in poll() waiting for
 * the X server to release an image -- measured at ~28% of the game thread on
 * crimecity2.  MAILBOX/FIFO use Mesa's own present thread instead.  Prefer
 * MAILBOX (no blocking, no tearing, no vsync cap), then IMMEDIATE, then FIFO
 * (always available).  VKSKELETON_PRESENT_MODE=mailbox|immediate|fifo forces
 * one if the driver supports it. */
static VkPresentModeKHR choose_present_mode(void)
{
    VkPresentModeKHR modes[16];
    uint32_t count = 16, i;
    const char *want = getenv("VKSKELETON_PRESENT_MODE");
    VkPresentModeKHR forced = (VkPresentModeKHR)-1;
    int has_mailbox = 0, has_immediate = 0;
    static int logged;
    VkPresentModeKHR chosen;

    if (!p_vkGetPhysicalDeviceSurfacePresentModesKHR ||
        p_vkGetPhysicalDeviceSurfacePresentModesKHR(physdev, surface, &count,
                                                    modes) < 0)
        count = 0;
    if (want && !strcmp(want, "mailbox"))
        forced = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (want && !strcmp(want, "immediate"))
        forced = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (want && !strcmp(want, "fifo"))
        forced = VK_PRESENT_MODE_FIFO_KHR;
    for (i = 0; i < count; i++) {
        if (modes[i] == forced) {
            if (!logged) {
                logged = 1;
                logf_msg("vkskeleton: present mode %s (forced by "
                         "VKSKELETON_PRESENT_MODE)\n", want);
            }
            return forced;
        }
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            has_mailbox = 1;
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            has_immediate = 1;
    }
    chosen = has_mailbox ? VK_PRESENT_MODE_MAILBOX_KHR
             : has_immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                             : VK_PRESENT_MODE_FIFO_KHR;
    if (!logged) {
        logged = 1;
        logf_msg("vkskeleton: present mode %s (mailbox=%d immediate=%d)\n",
                 chosen == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                 : chosen == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                           : "fifo",
                 has_mailbox, has_immediate);
    }
    return chosen;
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSwapchainCreateInfoKHR swci = {0};
    VkSwapchainKHR old = swapchain;
    VkExtent2D extent;
    VkResult r;
    uint32_t i;

    r = p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physdev, surface, &caps);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: GetSurfaceCapabilities failed (%d)\n", (int)r);
        return 0;
    }
    if (caps.currentExtent.width != 0xffffffffu) {
        extent = caps.currentExtent;
    } else {
        XWindowAttributes attrs;
        if (!XGetWindowAttributes(vk_dpy, vk_win, &attrs))
            return 0;
        extent.width = (uint32_t)attrs.width;
        extent.height = (uint32_t)attrs.height;
        if (extent.width < caps.minImageExtent.width)
            extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height)
            extent.height = caps.minImageExtent.height;
        if (caps.maxImageExtent.width && extent.width > caps.maxImageExtent.width)
            extent.width = caps.maxImageExtent.width;
        if (caps.maxImageExtent.height && extent.height > caps.maxImageExtent.height)
            extent.height = caps.maxImageExtent.height;
    }
    if (!extent.width || !extent.height)
        return -1;

    swci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swci.surface = surface;
    /* +2: one image being shown, one queued, one being drawn, with
     * FRAMES_IN_FLIGHT = 2 recording ahead -- so acquire rarely waits. */
    swci.minImageCount = caps.minImageCount + 2;
    if (caps.maxImageCount && swci.minImageCount > caps.maxImageCount)
        swci.minImageCount = caps.maxImageCount;
    swci.imageFormat = vk_surface_format.format;
    swci.imageColorSpace = vk_surface_format.colorSpace;
    swci.imageExtent = extent;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swap_can_copy = (caps.supportedUsageFlags &
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (swap_can_copy) /* glCopyTexSubImage2D emulation (mirrors) */
        swci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = choose_present_mode();
    swci.clipped = VK_TRUE;
    swci.oldSwapchain = old;
    r = p_vkCreateSwapchainKHR(device, &swci, NULL, &swapchain);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: CreateSwapchainKHR failed (%d)\n", (int)r);
        swapchain = old;
        return 0;
    }
    if (old)
        p_vkDestroySwapchainKHR(device, old, NULL);
    swap_extent = extent;
    swap_format = vk_surface_format.format;
    swap_image_count = 0;
    p_vkGetSwapchainImagesKHR(device, swapchain, &swap_image_count, NULL);
    if (swap_image_count > 16)
        swap_image_count = 16;
    p_vkGetSwapchainImagesKHR(device, swapchain, &swap_image_count, swap_images);
    if (p_vkCreateSemaphore) {
        VkSemaphoreCreateInfo semi = {0};
        semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (i = 0; i < swap_image_count; i++) {
            if (!render_sems[i] &&
                p_vkCreateSemaphore(device, &semi, NULL, &render_sems[i]) !=
                    VK_SUCCESS)
                return 0;
        }
    }
    logf_msg("vkskeleton: swap extent %ux%u format=%d images=%u\n",
             swap_extent.width, swap_extent.height, (int)swap_format,
             swap_image_count);
    return 1;
}

/* Window resized (or minimized/restored): rebuild everything sized to the
 * swapchain.  Pipelines use dynamic viewport/scissor and survive. */
static int recreate_swapchain(void)
{
    int rc;

    pthread_mutex_lock(&queue_mtx);
    p_vkDeviceWaitIdle(device);
    pthread_mutex_unlock(&queue_mtx);
    completed_serial = frame_serial;
    destroy_swap_resources();
    rc = create_swapchain();
    if (rc <= 0) {
        swapchain_needs_rebuild = 1; /* retry next frame */
        return rc;
    }
    if (!create_swap_resources()) {
        logmsg("vkskeleton: swapchain resources failed after resize\n");
        swapchain_dead = 1;
        return 0;
    }
    swapchain_needs_rebuild = 0;
    return 1;
}

static int window_size_changed(void)
{
    VkSurfaceCapabilitiesKHR caps;

    if (p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physdev, surface, &caps) !=
        VK_SUCCESS)
        return 0;
    if (caps.currentExtent.width == 0xffffffffu)
        return 0;
    return caps.currentExtent.width != swap_extent.width ||
           caps.currentExtent.height != swap_extent.height;
}

static int init_vulkan(Display *dpy, Window win)
{
    VkApplicationInfo app = {0};
    VkInstanceCreateInfo ici = {0};
    const char *inst_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
    };
    uint32_t dev_count = 0;
    VkPhysicalDevice *devices;
    uint32_t qfam_count = 0;
    VkQueueFamilyProperties *qfams;
    uint32_t chosen = ~0u;
    VkSurfaceCapabilitiesKHR caps;
    uint32_t fmt_count = 0;
    VkSurfaceFormatKHR *formats;
    VkSurfaceFormatKHR chosen_format;
    VkDeviceQueueCreateInfo qci = {0};
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {0};
    float priority = 1.0f;
    VkXlibSurfaceCreateInfoKHR sci = {0};
    VkSwapchainCreateInfoKHR swci = {0};
    VkCommandPoolCreateInfo cpci = {0};
    VkCommandBufferAllocateInfo cbai = {0};
    VkSemaphoreCreateInfo semi = {0};
    VkFenceCreateInfo fi = {0};
    VkResult r;
    uint32_t i;
    XWindowAttributes attrs;

    logmsg("vkskeleton: init\n");
    vulkan_lib = dlopen("libvulkan.so.1", RTLD_LAZY);
    if (!vulkan_lib) {
        logmsg("vkskeleton: dlopen libvulkan failed\n");
        return 0;
    }
    fp_vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)real_dlsym(vulkan_lib,
                                             "vkGetInstanceProcAddr");
    fp_vkGetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)real_dlsym(vulkan_lib, "vkGetDeviceProcAddr");
    if (!fp_vkGetInstanceProcAddr || !fp_vkGetDeviceProcAddr) {
        logmsg("vkskeleton: missing Vulkan loader entry points\n");
        return 0;
    }

#define LOAD_INSTANCE_CORE()                                                   \
    do {                                                                       \
        LOAD_INSTance(CreateInstance);                                         \
        LOAD_INSTance(EnumeratePhysicalDevices);                               \
        LOAD_INSTance(GetPhysicalDeviceQueueFamilyProperties);                 \
        LOAD_INSTance(GetPhysicalDeviceMemoryProperties);                      \
        LOAD_INSTance(GetPhysicalDeviceFeatures);                              \
        LOAD_INSTance(GetPhysicalDeviceSurfaceSupportKHR);                     \
        LOAD_INSTance(GetPhysicalDeviceSurfaceCapabilitiesKHR);                \
        LOAD_INSTance(GetPhysicalDeviceSurfaceFormatsKHR);                     \
        LOAD_INSTance(GetPhysicalDeviceSurfacePresentModesKHR);                \
        LOAD_INSTance(CreateXlibSurfaceKHR);                                   \
        LOAD_INSTance(CreateDevice);                                           \
        LOAD_INSTance(GetDeviceQueue);                                         \
    } while (0)

    LOAD_INSTANCE_CORE();

    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "svencoop-vkskeleton";
    app.applicationVersion = 1;
    app.pEngineName = "vkskeleton";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_1; /* external memory core (vkx modules) */
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;
    r = p_vkCreateInstance(&ici, NULL, &instance);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: CreateInstance failed (%d)\n", (int)r);
        return 0;
    }
    LOAD_INSTANCE_CORE();
    logmsg("vkskeleton: instance ready\n");

    sci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    sci.dpy = dpy;
    sci.window = win;
    r = p_vkCreateXlibSurfaceKHR(instance, &sci, NULL, &surface);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: CreateXlibSurfaceKHR failed (%d)\n", (int)r);
        return 0;
    }
    logmsg("vkskeleton: surface ready\n");

    p_vkEnumeratePhysicalDevices(instance, &dev_count, NULL);
    if (!dev_count) {
        logmsg("vkskeleton: no physical devices\n");
        return 0;
    }
    devices = calloc(dev_count, sizeof(*devices));
    p_vkEnumeratePhysicalDevices(instance, &dev_count, devices);

    for (i = 0; i < dev_count; i++) {
        p_vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qfam_count,
                                                   NULL);
        qfams = calloc(qfam_count ? qfam_count : 1, sizeof(*qfams));
        p_vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qfam_count,
                                                   qfams);
        for (uint32_t j = 0; j < qfam_count; j++) {
            VkBool32 supported = VK_FALSE;
            if (!(qfams[j].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            p_vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], j, surface,
                                                    &supported);
            if (supported) {
                physdev = devices[i];
                chosen = j;
                free(qfams);
                break;
            }
        }
        if (chosen != ~0u)
            break;
        free(qfams);
    }
    free(devices);
    if (chosen == ~0u) {
        logmsg("vkskeleton: no suitable queue family\n");
        return 0;
    }
    logmsg("vkskeleton: physical device selected\n");

    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = chosen;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    {
        /* swapchain + whatever a linked vkx module asks for */
        static const char *all_exts[16];
        uint32_t n = 0;
        const char *const *extra = vkx_device_extensions ? vkx_device_extensions()
                                                         : NULL;
        all_exts[n++] = dev_exts[0];
        while (extra && *extra && n < 16)
            all_exts[n++] = *extra++;
        dci.enabledExtensionCount = n;
        dci.ppEnabledExtensionNames = all_exts;
    }
    {
        /* gl_ClipDistance emulates glClipPlane (mirrors cut away what is
         * behind the mirror plane). */
        static VkPhysicalDeviceFeatures want;
        VkPhysicalDeviceFeatures have;
        memset(&have, 0, sizeof(have));
        p_vkGetPhysicalDeviceFeatures(physdev, &have);
        memset(&want, 0, sizeof(want));
        want.shaderClipDistance = have.shaderClipDistance;
        if (!have.shaderClipDistance)
            logmsg("vkskeleton: WARNING: shaderClipDistance unsupported\n");
        dci.pEnabledFeatures = &want;
    }
    r = p_vkCreateDevice(physdev, &dci, NULL, &device);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: CreateDevice failed (%d)\n", (int)r);
        return 0;
    }
    p_vkGetDeviceQueue(device, chosen, 0, &queue);
    logmsg("vkskeleton: device ready\n");

    LOAD_DEVICE(CreateSwapchainKHR);
    LOAD_DEVICE(GetSwapchainImagesKHR);
    LOAD_DEVICE(CreateImageView);
    LOAD_DEVICE(CreateImage);
    LOAD_DEVICE(GetImageMemoryRequirements);
    LOAD_DEVICE(BindImageMemory);
    LOAD_DEVICE(CmdCopyBufferToImage);
    LOAD_DEVICE(DestroyImage);
    LOAD_DEVICE(DestroyImageView);
    LOAD_DEVICE(FreeMemory);
    LOAD_DEVICE(CreateDescriptorSetLayout);
    LOAD_DEVICE(CreateDescriptorPool);
    LOAD_DEVICE(AllocateDescriptorSets);
    LOAD_DEVICE(UpdateDescriptorSets);
    LOAD_DEVICE(CreateSampler);
    LOAD_DEVICE(CreateRenderPass);
    LOAD_DEVICE(CreateShaderModule);
    LOAD_DEVICE(CreatePipelineLayout);
    LOAD_DEVICE(CreateGraphicsPipelines);
    LOAD_DEVICE(CreateFramebuffer);
    LOAD_DEVICE(CreateBuffer);
    LOAD_DEVICE(GetBufferMemoryRequirements);
    LOAD_DEVICE(AllocateMemory);
    LOAD_DEVICE(MapMemory);
    LOAD_DEVICE(UnmapMemory);
    LOAD_DEVICE(BindBufferMemory);
    LOAD_DEVICE(CreateCommandPool);
    LOAD_DEVICE(AllocateCommandBuffers);
    LOAD_DEVICE(BeginCommandBuffer);
    LOAD_DEVICE(CmdPipelineBarrier);
    LOAD_DEVICE(CmdClearColorImage);
    LOAD_DEVICE(CmdBeginRenderPass);
    LOAD_DEVICE(CmdEndRenderPass);
    LOAD_DEVICE(CmdBindPipeline);
    LOAD_DEVICE(CmdBindDescriptorSets);
    LOAD_DEVICE(CmdBindVertexBuffers);
    LOAD_DEVICE(CmdBindIndexBuffer);
    LOAD_DEVICE(CmdDrawIndexed);
    LOAD_DEVICE(CmdDraw);
    LOAD_DEVICE(EndCommandBuffer);
    LOAD_DEVICE(CreateSemaphore);
    LOAD_DEVICE(CreateFence);
    LOAD_DEVICE(ResetFences);
    LOAD_DEVICE(WaitForFences);
    LOAD_DEVICE(AcquireNextImageKHR);
    LOAD_DEVICE(QueueSubmit);
    LOAD_DEVICE(QueuePresentKHR);
    LOAD_DEVICE(QueueWaitIdle);
    LOAD_DEVICE(DestroyFramebuffer);
    LOAD_DEVICE(DestroySwapchainKHR);
    LOAD_DEVICE(DeviceWaitIdle);
    LOAD_DEVICE(CmdSetViewport);
    LOAD_DEVICE(CmdSetScissor);
    LOAD_DEVICE(DestroyRenderPass);
    LOAD_DEVICE(CmdPushConstants);
    LOAD_DEVICE(CmdBlitImage);
    LOAD_DEVICE(CmdClearAttachments);
    LOAD_DEVICE(CmdSetStencilReference);
    LOAD_DEVICE(CmdSetDepthBias);
    logmsg("vkskeleton: device procs ready\n");
    if (vkx_on_device)
        vkx_on_device(instance, physdev, device, queue, chosen,
                      fp_vkGetDeviceProcAddr);

    r = p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physdev, surface, &caps);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: GetSurfaceCapabilities failed (%d)\n", (int)r);
        return 0;
    }
    r = p_vkGetPhysicalDeviceSurfaceFormatsKHR(physdev, surface, &fmt_count,
                                               NULL);
    if (r != VK_SUCCESS || !fmt_count) {
        logf_msg("vkskeleton: GetSurfaceFormats failed (%d/%u)\n", (int)r,
                 fmt_count);
        return 0;
    }
    formats = calloc(fmt_count, sizeof(*formats));
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(physdev, surface, &fmt_count,
                                           formats);
    chosen_format = formats[0];
    for (i = 0; i < fmt_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen_format = formats[i];
            break;
        }
    }
    free(formats);
    logmsg("vkskeleton: surface info ready\n");

    vk_dpy = dpy;
    vk_win = win;
    vk_surface_format = chosen_format;
    (void)attrs;
    (void)swci;
    if (create_swapchain() <= 0)
        return 0;
    logmsg("vkskeleton: swapchain ready\n");

    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    /* Command buffers are re-recorded every frame. */
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = chosen;
    r = p_vkCreateCommandPool(device, &cpci, NULL, &cmd_pool);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: CreateCommandPool failed (%d)\n", (int)r);
        return 0;
    }
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = FRAMES_IN_FLIGHT;
    r = p_vkAllocateCommandBuffers(device, &cbai, cmd_buffers);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: AllocateCommandBuffers failed (%d)\n", (int)r);
        return 0;
    }

    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    {
        uint32_t k;
        for (k = 0; k < FRAMES_IN_FLIGHT; k++) {
            if (p_vkCreateSemaphore(device, &semi, NULL, &acquire_sems[k]) !=
                    VK_SUCCESS ||
                p_vkCreateFence(device, &fi, NULL, &frame_fences[k]) !=
                    VK_SUCCESS) {
                logmsg("vkskeleton: semaphore/fence failed\n");
                return 0;
            }
        }
        for (k = 0; k < swap_image_count; k++) {
            if (!render_sems[k] &&
                p_vkCreateSemaphore(device, &semi, NULL, &render_sems[k]) !=
                    VK_SUCCESS) {
                logmsg("vkskeleton: render semaphore failed\n");
                return 0;
            }
        }
    }
    logmsg("vkskeleton: sync ready\n");

    if (!create_graphics_resources())
        return 0;

    ready = 1;
    logmsg("vkskeleton: ready\n");
    return 1;
}

static int init_vulkan_captured(void)
{
    Display *dpy = NULL;
    Window win = None;

    if (ready || init_attempted)
        return ready;
    init_attempted = 1;

    if (!choose_window(&dpy, &win))
        return 0;
    return init_vulkan(dpy, win);
}

/* ---------- frame recording / presentation ---------- */

static int begin_frame(void)
{
    VkRenderPassBeginInfo rpbi = {0};
    VkClearValue clear_values[2] = {{0}, {0}};
    VkResult r;

    if (frame_active)
        return 1;
    if (!ready)
        return 0;
    if (swapchain_dead)
        return 0;

    {
        /* Resize detection: OUT_OF_DATE/SUBOPTIMAL set the flag; a cheap
         * periodic size check catches drivers that report neither. */
        static unsigned size_check;
        if (!swapchain_needs_rebuild && (++size_check % 30) == 0 &&
            window_size_changed())
            swapchain_needs_rebuild = 1;
    }
    if (swapchain_needs_rebuild && recreate_swapchain() <= 0)
        return 0;

    /* Reuse this slot only once the GPU has finished its previous frame. */
    p_vkWaitForFences(device, 1, &frame_fences[rec_slot], VK_TRUE, UINT64_MAX);
    if (frame_slot_serial[rec_slot] > completed_serial)
        completed_serial = frame_slot_serial[rec_slot];

    r = p_vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                acquire_sems[rec_slot], VK_NULL_HANDLE,
                                &current_image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        /* Resized between frames: rebuild now and retry once. */
        if (recreate_swapchain() <= 0)
            return 0;
        r = p_vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                    acquire_sems[rec_slot], VK_NULL_HANDLE,
                                    &current_image);
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        swapchain_needs_rebuild = 1; /* image is usable; rebuild next frame */
    } else if (r != VK_SUCCESS) {
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
            swapchain_needs_rebuild = 1;
        else
            logf_msg("vkskeleton: AcquireNextImageKHR failed (%d)\n", (int)r);
        return 0;
    }

    p_vkResetFences(device, 1, &frame_fences[rec_slot]);
    p_vkBeginCommandBuffer(cmd_buffers[rec_slot],
                           &(VkCommandBufferBeginInfo){
                               .sType =
                                   VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                               .flags =
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                           });

    clear_values[0].color.float32[0] = rec_clear_color[0];
    clear_values[0].color.float32[1] = rec_clear_color[1];
    clear_values[0].color.float32[2] = rec_clear_color[2];
    clear_values[0].color.float32[3] = rec_clear_color[3];
    clear_values[1].depthStencil.depth = 1.0f;
    clear_values[1].depthStencil.stencil = 0;
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = render_pass;
    rpbi.framebuffer = framebuffers[current_image];
    rpbi.renderArea.offset.x = 0;
    rpbi.renderArea.offset.y = 0;
    rpbi.renderArea.extent = swap_extent;
    rpbi.clearValueCount = 2;
    rpbi.pClearValues = clear_values;
    p_vkCmdBeginRenderPass(cmd_buffers[rec_slot], &rpbi,
                           VK_SUBPASS_CONTENTS_INLINE);
    {
        VkViewport vp = {0};
        VkRect2D sc = {{0, 0}, {0, 0}};
        vp.width = (float)swap_extent.width;
        vp.height = (float)swap_extent.height;
        vp.maxDepth = 1.0f;
        sc.extent = swap_extent;
        p_vkCmdSetViewport(cmd_buffers[rec_slot], 0, 1, &vp);
        p_vkCmdSetScissor(cmd_buffers[rec_slot], 0, 1, &sc);
    }
    frame_active = 1;
    return 1;
}



/* ---------- pipelines keyed by full fixed-function state ----------
 * bits 0-1 blend mode | 2 depth test | 3 depth write | 4 colour write |
 * 5 stencil test | 6-8 stencil compare | 9-11 sfail | 12-14 zfail |
 * 15-17 zpass | 18-20 depth compare | 21 depth bias (glPolygonOffset).
 * GL compare enums 0x200..0x207 map
 * 1:1 onto VkCompareOp 0..7.  Created on first use and cached. */
static uint32_t gl_stencil_op_to_vk(GLenum op)
{
    switch (op) {
    case 0:      return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;
    }
}

#define PKEY_HUD_ALPHA (1u << 22) /* HUD target alpha, see create_pipeline_for_key */
static uint32_t pipeline_key(int blend)
{
    uint32_t k = (uint32_t)blend & 3u;

    if (depth_test_enabled) {
        k |= 1u << 2;
        if (depth_write_enabled)
            k |= 1u << 3;
        k |= ((uint32_t)(gl_depth_func - 0x200) & 7u) << 18;
    }
    if (!color_mask_off)
        k |= 1u << 4;
    if (depth_test_enabled && polygon_offset_enabled)
        k |= 1u << 21;
    if (stencil_test_enabled) {
        k |= 1u << 5;
        k |= ((uint32_t)(st_func - 0x200) & 7u) << 6;
        k |= gl_stencil_op_to_vk(st_sfail) << 9;
        k |= gl_stencil_op_to_vk(st_zfail) << 12;
        k |= gl_stencil_op_to_vk(st_zpass) << 15;
    }
    return k;
}

static VkPipeline create_pipeline_for_key(uint32_t k)
{
    static const VkBlendFactor bsrc[4] = {
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE};
    static const VkBlendFactor bdst[4] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE};
    static const VkDynamicState dyn_states[4] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription binding = {0};
    VkVertexInputAttributeDescription attrs[5];
    VkPipelineVertexInputStateCreateInfo visa = {0};
    VkPipelineInputAssemblyStateCreateInfo pia = {0};
    VkPipelineViewportStateCreateInfo pvs = {0};
    VkPipelineRasterizationStateCreateInfo prs = {0};
    VkPipelineMultisampleStateCreateInfo pms = {0};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cbs = {0};
    VkPipelineDepthStencilStateCreateInfo dss = {0};
    VkPipelineDynamicStateCreateInfo dyn = {0};
    VkGraphicsPipelineCreateInfo gpci = {0};
    VkStencilOpState so = {0};
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t blend = k & 3u;
    VkResult r;

    memset(stages, 0, sizeof(stages));
    memset(attrs, 0, sizeof(attrs));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    binding.stride = sizeof(vk_vertex_t);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(vk_vertex_t, x);
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(vk_vertex_t, u);
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[2].offset = offsetof(vk_vertex_t, r);
    attrs[3].location = 3;
    attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[3].offset = offsetof(vk_vertex_t, alpha_ref);
    attrs[4].location = 4;
    attrs[4].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[4].offset = offsetof(vk_vertex_t, u2);
    visa.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    visa.vertexBindingDescriptionCount = 1;
    visa.pVertexBindingDescriptions = &binding;
    visa.vertexAttributeDescriptionCount = 5;
    visa.pVertexAttributeDescriptions = attrs;

    pia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    pia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pvs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    pvs.viewportCount = 1;
    pvs.scissorCount = 1;
    prs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    prs.polygonMode = VK_POLYGON_MODE_FILL;
    prs.cullMode = VK_CULL_MODE_NONE;
    prs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    prs.lineWidth = 1.0f;
    /* glPolygonOffset: GL "units" and "factor" correspond to Vulkan's
     * constant and slope factors (both scaled by the minimum resolvable
     * depth difference); values are dynamic state per batch. */
    prs.depthBiasEnable = (k & (1u << 21)) ? VK_TRUE : VK_FALSE;
    pms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    pms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    if (k & (1u << 4))
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend != BLEND_OPAQUE) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = bsrc[blend];
        cba.dstColorBlendFactor = bdst[blend];
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = bsrc[blend];
        cba.dstAlphaBlendFactor = bdst[blend];
        if (k & PKEY_HUD_ALPHA) {
            /* HUD target: alpha = coverage for premultiplied composition;
             * additive draws add light without covering what is behind */
            int additive = bdst[blend] == VK_BLEND_FACTOR_ONE;
            cba.srcAlphaBlendFactor = additive ? VK_BLEND_FACTOR_ZERO
                                               : VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = additive ? VK_BLEND_FACTOR_ONE
                                               : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = (k & (1u << 2)) ? VK_TRUE : VK_FALSE;
    dss.depthWriteEnable = (k & (1u << 3)) ? VK_TRUE : VK_FALSE;
    dss.depthCompareOp = (VkCompareOp)((k >> 18) & 7u);
    if (k & (1u << 5)) {
        dss.stencilTestEnable = VK_TRUE;
        so.compareOp = (VkCompareOp)((k >> 6) & 7u);
        so.failOp = (VkStencilOp)((k >> 9) & 7u);
        so.depthFailOp = (VkStencilOp)((k >> 12) & 7u);
        so.passOp = (VkStencilOp)((k >> 15) & 7u);
        so.compareMask = 0xff;
        so.writeMask = 0xff;
        dss.front = so;
        dss.back = so;
    }

    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 4;
    dyn.pDynamicStates = dyn_states;

    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &visa;
    gpci.pInputAssemblyState = &pia;
    gpci.pViewportState = &pvs;
    gpci.pRasterizationState = &prs;
    gpci.pMultisampleState = &pms;
    gpci.pDepthStencilState = &dss;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dyn;
    gpci.layout = pipeline_layout;
    gpci.renderPass = render_pass; /* compatible with render_pass_load */
    gpci.subpass = 0;
    r = p_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL,
                                    &pipe);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: pipeline key 0x%x failed (%d)\n", k, (int)r);
        return VK_NULL_HANDLE;
    }
    if (debug_enabled)
        logf_msg("vkskeleton: dbg pipeline key 0x%05x created\n", k);
    return pipe;
}

#define MAX_PIPELINE_CACHE 512
static struct { uint32_t key; VkPipeline pipe; } pipeline_cache[MAX_PIPELINE_CACHE];
static int pipeline_cache_count;

static VkPipeline get_pipeline(uint32_t k)
{
    static uint32_t last_key = ~0u;
    static VkPipeline last_pipe;
    int i;

    if (k == last_key && last_pipe)
        return last_pipe;
    for (i = 0; i < pipeline_cache_count; i++) {
        if (pipeline_cache[i].key == k) {
            last_key = k;
            last_pipe = pipeline_cache[i].pipe;
            return last_pipe;
        }
    }
    if (pipeline_cache_count >= MAX_PIPELINE_CACHE)
        return VK_NULL_HANDLE;
    pipeline_cache[pipeline_cache_count].key = k;
    pipeline_cache[pipeline_cache_count].pipe = create_pipeline_for_key(k);
    last_key = k;
    last_pipe = pipeline_cache[pipeline_cache_count++].pipe;
    return last_pipe;
}

static void image_barrier(VkCommandBuffer cb, VkImage image,
                          VkImageAspectFlags aspect, VkImageLayout from,
                          VkImageLayout to, VkAccessFlags src_access,
                          VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {0};

    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    p_vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---------- vkx eye views and HUD ----------
 * After the main pass, the frame's command list is replayed once per eye
 * into the eye target (eyes side by side) and, if the module wants the HUD
 * separate, once more with only the 2D draws into the HUD target
 * (transparent background, premultiplied alpha).  Targets use a render pass
 * compatible with render_pass (same formats), so pipelines are shared.
 * Colour stays in TRANSFER_SRC_OPTIMAL between passes (blit source for
 * mirror copies and for the module). */
#define VIEW_HUD 2                /* cur_eye value of the HUD replay */
typedef struct {
    VkImage image, depth;
    VkDeviceMemory mem, depth_mem;
    VkImageView view, depth_view;
    VkFramebuffer fb;
    VkExtent2D extent;
} view_target_t;
static view_target_t eye_tg, hud_tg, *cur_tg;
static VkRenderPass eye_pass;
static VkFormat eye_pass_format;
static void fix_clip(const float *m, float a, float b, float *o);
static void replay_cmds(VkCommandBuffer cb);

static int target_alloc(view_target_t *t, VkFormat fmt, VkImageUsageFlags usage,
                        VkImageAspectFlags aspect, VkImage *img,
                        VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ci = {0};
    VkImageViewCreateInfo vi = {0};
    VkMemoryAllocateInfo ai = {0};
    VkMemoryRequirements req;

    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = t->extent.width;
    ci.extent.height = t->extent.height;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(device, &ci, NULL, img) != VK_SUCCESS)
        return 0;
    p_vkGetImageMemoryRequirements(device, *img, &req);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u ||
        p_vkAllocateMemory(device, &ai, NULL, mem) != VK_SUCCESS ||
        p_vkBindImageMemory(device, *img, *mem, 0) != VK_SUCCESS)
        return 0;
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return p_vkCreateImageView(device, &vi, NULL, view) == VK_SUCCESS;
}

static void target_destroy(view_target_t *t)
{
    if (t->fb) p_vkDestroyFramebuffer(device, t->fb, NULL);
    if (t->view) p_vkDestroyImageView(device, t->view, NULL);
    if (t->depth_view) p_vkDestroyImageView(device, t->depth_view, NULL);
    if (t->image) p_vkDestroyImage(device, t->image, NULL);
    if (t->depth) p_vkDestroyImage(device, t->depth, NULL);
    if (t->mem) p_vkFreeMemory(device, t->mem, NULL);
    if (t->depth_mem) p_vkFreeMemory(device, t->depth_mem, NULL);
    memset(t, 0, sizeof(*t));
}

/* (Re)create target `t` at `want`; called on the recording thread. */
static int target_resources(view_target_t *t, VkExtent2D want)
{
    VkAttachmentDescription att[2];
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0};
    VkRenderPassCreateInfo rpci = {0};
    VkFramebufferCreateInfo fci = {0};
    VkImageView views[2];

    if (t->fb && t->extent.width == want.width &&
        t->extent.height == want.height && eye_pass_format == swap_format)
        return 1;
    pthread_mutex_lock(&queue_mtx);
    p_vkDeviceWaitIdle(device);
    pthread_mutex_unlock(&queue_mtx);
    target_destroy(t);
    if (eye_pass && eye_pass_format != swap_format) {
        /* shortcut: the other target keeps a framebuffer of the old pass
         * until its own size check recreates it; formats rarely change */
        target_destroy(t == &eye_tg ? &hud_tg : &eye_tg);
        p_vkDestroyRenderPass(device, eye_pass, NULL);
        eye_pass = VK_NULL_HANDLE;
    }
    if (!eye_pass) {
        memset(att, 0, sizeof(att));
        att[0].format = swap_format;
        att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        att[1] = att[0];
        att[1].format = VK_DEPTH_FORMAT;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        sub.pDepthStencilAttachment = &dref;
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 2;
        rpci.pAttachments = att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        if (p_vkCreateRenderPass(device, &rpci, NULL, &eye_pass) != VK_SUCCESS)
            return 0;
        eye_pass_format = swap_format;
    }
    t->extent = want;
    if (!target_alloc(t, swap_format,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, &t->image, &t->mem, &t->view) ||
        !target_alloc(t, VK_DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                      &t->depth, &t->depth_mem, &t->depth_view)) {
        logmsg("vkskeleton: view target allocation failed\n");
        target_destroy(t);
        return 0;
    }
    views[0] = t->view;
    views[1] = t->depth_view;
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = eye_pass;
    fci.attachmentCount = 2;
    fci.pAttachments = views;
    fci.width = want.width;
    fci.height = want.height;
    fci.layers = 1;
    if (p_vkCreateFramebuffer(device, &fci, NULL, &t->fb) != VK_SUCCESS) {
        target_destroy(t);
        return 0;
    }
    logf_msg("vkskeleton: %s target %ux%u\n", t == &hud_tg ? "HUD" : "eye",
             want.width, want.height);
    return 1;
}

/* Begin (or resume after a mirror copy) the pass on cur_tg / cur_rect. */
static void eye_begin(VkCommandBuffer cb, int clear)
{
    VkRenderPassBeginInfo rpbi = {0};
    VkViewport vp = {0};
    VkDeviceSize offset = (VkDeviceSize)rec_slot * FRAME_VERTEX_BYTES;

    image_barrier(cb, cur_tg->image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    image_barrier(cb, cur_tg->depth,
                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = eye_pass;
    rpbi.framebuffer = cur_tg->fb;
    rpbi.renderArea = cur_rect;
    p_vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vp.x = (float)cur_rect.offset.x;
    vp.y = (float)cur_rect.offset.y;
    vp.width = (float)cur_rect.extent.width;
    vp.height = (float)cur_rect.extent.height;
    vp.maxDepth = 1.0f;
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    p_vkCmdSetScissor(cb, 0, 1, &cur_rect);
    p_vkCmdBindVertexBuffers(cb, 0, 1, &vertex_buffer, &offset);
    p_vkCmdBindIndexBuffer(cb, index_buffer,
                           (VkDeviceSize)rec_slot * FRAME_INDEX_BYTES,
                           VK_INDEX_TYPE_UINT32);
    if (clear) { /* what render_pass's CLEAR load does for the swapchain */
        VkClearAttachment ca[2];
        VkClearRect rect = {{{0, 0}, {0, 0}}, 0, 1};
        memset(ca, 0, sizeof(ca));
        ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        if (cur_eye != VIEW_HUD) /* HUD: transparent */
            memcpy(ca[0].clearValue.color.float32, rec_clear_color,
                   sizeof(rec_clear_color));
        ca[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        ca[1].clearValue.depthStencil.depth = 1.0f;
        rect.rect = cur_rect;
        p_vkCmdClearAttachments(cb, 2, ca, 1, &rect);
    }
}

/* Is draw matrix `mi` a perspective (3D) one? */
static int mat_is_3d(uint32_t mi) { return rec_mats[mi][20 + 11] < -0.5f; }

/* Push constants of draw matrix `mi` for the current replay target. */
static const float *replay_mat(uint32_t mi, float *out)
{
    const float *s = rec_mats[mi];
    const vkx_eye_t *e;
    mat4 p, t, mv, ev;
    float l, r, d, u;

    if (cur_eye < 0 || cur_eye == VIEW_HUD || !mat_is_3d(mi))
        return s;
    e = &rec_eyes[cur_eye];
    l = e->tan[0]; r = e->tan[1]; d = e->tan[2]; u = e->tan[3];
    memset(&p, 0, sizeof(p));
    p.m[0] = 2.0f / (r - l);
    p.m[5] = 2.0f / (u - d);
    p.m[8] = (r + l) / (r - l);
    p.m[9] = (u + d) / (u - d);
    p.m[10] = s[20 + 10]; /* the game's near/far mapping */
    p.m[11] = -1.0f;
    p.m[14] = s[20 + 14];
    memcpy(mv.m, s + 36, sizeof(mv.m));
    memcpy(ev.m, e->eye, sizeof(ev.m));
    /* viewmodel: drawn with a compressed glDepthRange (GoldSrc: 0..0.3) */
    if (rec_vm_valid && s[52] < 0.45f) {
        static int logged;
        mat4 vm;
        memcpy(vm.m, rec_vm, sizeof(vm.m));
        mat_mul(&mv, &vm, &mv);
        if (!logged) {
            logged = 1;
            logmsg("vkskeleton: vkx viewmodel draws found\n");
        }
    }
    mat_mul(&t, &ev, &mv);
    mat_mul(&t, &p, &t);
    fix_clip(t.m, s[52], s[53], out);
    memcpy(out + 16, s + 16, 4 * sizeof(float));
    return out;
}

/* Start replaying into target t (previous contents discarded). */
static void target_start(VkCommandBuffer cb, view_target_t *t)
{
    image_barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  0, 0, VK_PIPELINE_STAGE_TRANSFER_BIT |
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    image_barrier(cb, t->depth,
                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    cur_tg = t;
}

/* Replay the frame per eye (and the HUD); returns 1 if the eye target
 * holds the eyes, *hud set if the HUD target holds the HUD. */
static int record_eyes(VkCommandBuffer cb, int *hud)
{
    VkExtent2D want;
    int e;

    *hud = 0;
    if (!rec_eye_count || !rec_cmd_count || !vertex_map)
        return 0;
    want.width = rec_eye_extent.width * (uint32_t)rec_eye_count;
    want.height = rec_eye_extent.height;
    if (!target_resources(&eye_tg, want))
        return 0;
    target_start(cb, &eye_tg);
    for (e = 0; e < rec_eye_count; e++) {
        cur_eye = e;
        cur_rect.offset.x = (int32_t)(rec_eye_extent.width * (uint32_t)e);
        cur_rect.offset.y = 0;
        cur_rect.extent = rec_eye_extent;
        eye_begin(cb, 1);
        replay_cmds(cb);
        p_vkCmdEndRenderPass(cb);
    }
    if (rec_hud && target_resources(&hud_tg, swap_extent)) {
        target_start(cb, &hud_tg);
        cur_eye = VIEW_HUD;
        cur_rect.offset.x = cur_rect.offset.y = 0;
        cur_rect.extent = swap_extent;
        eye_begin(cb, 1);
        replay_cmds(cb);
        p_vkCmdEndRenderPass(cb);
        *hud = 1;
    }
    cur_eye = -1;
    return 1;
}

/* glCopyTexSubImage2D: end the render pass, blit the swapchain image into
 * the texture, resume in a LOAD render pass.  GL framebuffer rows run
 * bottom-up while the Vulkan image is top-down; textures keep GL row order,
 * so the source rectangle is flipped vertically in the blit. */
static void record_framebuffer_copy(VkCommandBuffer cb, const frame_cmd_t *c)
{
    vk_texture_t *tex = &vk_textures[c->a];
    int32_t W = (int32_t)swap_extent.width, H = (int32_t)swap_extent.height;
    int32_t w = c->w, h = c->h;
    VkImageBlit blit = {0};
    VkRenderPassBeginInfo rpbi = {0};
    VkViewport vp = {0};
    VkRect2D sc = {{0, 0}, {0, 0}};
    /* source: the swapchain, or the current eye area (scaled from GL
     * window coordinates); an eye image is already TRANSFER_SRC */
    int eye = cur_eye >= 0;
    VkImage src = eye ? cur_tg->image : swap_images[current_image];
    VkImageLayout sl = eye ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                           : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    float fx = (float)cur_rect.extent.width / (float)W;
    float fy = (float)cur_rect.extent.height / (float)H;
    int32_t ox = cur_rect.offset.x, oy = cur_rect.offset.y;

    p_vkCmdEndRenderPass(cb);

    if (c->sx < 0 || c->sy < 0 || c->dx < 0 || c->dy < 0)
        w = h = 0;
    if (c->sx + w > W) w = W - c->sx;
    if (c->sy + h > H) h = H - c->sy;
    if (tex->image && c->dx + w > (int32_t)tex->width) w = (int32_t)tex->width - c->dx;
    if (tex->image && c->dy + h > (int32_t)tex->height) h = (int32_t)tex->height - c->dy;

    if (tex->image && w > 0 && h > 0) {
        image_barrier(cb, src, VK_IMAGE_ASPECT_COLOR_BIT, sl,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        image_barrier(cb, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                      tex->valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0].x = ox + (int32_t)((float)c->sx * fx);
        blit.srcOffsets[0].y = oy + (int32_t)((float)(H - c->sy) * fy); /* GL bottom row */
        blit.srcOffsets[0].z = 0;
        blit.srcOffsets[1].x = ox + (int32_t)((float)(c->sx + w) * fx);
        blit.srcOffsets[1].y = oy + (int32_t)((float)(H - c->sy - h) * fy); /* GL top row */
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0].x = c->dx;
        blit.dstOffsets[0].y = c->dy;
        blit.dstOffsets[1].x = c->dx + w;
        blit.dstOffsets[1].y = c->dy + h;
        blit.dstOffsets[1].z = 1;
        p_vkCmdBlitImage(cb, src,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                         VK_FILTER_NEAREST);
        image_barrier(cb, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        image_barrier(cb, src, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sl,
                      VK_ACCESS_TRANSFER_READ_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        tex->valid = 1;
        tex->last_used_serial = frame_serial + 1;
    }
    if (eye) {
        eye_begin(cb, 0);
        return;
    }
    if (!(tex->image && w > 0 && h > 0)) {
        /* nothing copied: still make the colour writes visible to the
         * next (LOAD) pass, layout unchanged */
        image_barrier(cb, swap_images[current_image], VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    /* depth/stencil written by the first pass is read by the next one */
    image_barrier(cb, depth_image,
                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = render_pass_load;
    rpbi.framebuffer = framebuffers[current_image];
    rpbi.renderArea.extent = swap_extent;
    p_vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vp.width = (float)swap_extent.width;
    vp.height = (float)swap_extent.height;
    vp.maxDepth = 1.0f;
    sc.extent = swap_extent;
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    p_vkCmdSetScissor(cb, 0, 1, &sc);
}

static void record_frame_vertices(void)
{
    VkDeviceSize offset = 0;
    int k;

    if (!frame_active || !rec_cmd_count || !vertex_map)
        return;
    offset = (VkDeviceSize)rec_slot * FRAME_VERTEX_BYTES;

    if (!logged_first_draw) {
        logged_first_draw = 1;
        logf_msg("vkskeleton: first Vulkan draw (%u vertices)\n",
                 (unsigned)rec_vertex_count);
    }
    for (k = 0; k < logged_draw_size_count; k++) {
        if (logged_draw_sizes[k] == (int)rec_vertex_count)
            break;
    }
    if (k == logged_draw_size_count && logged_draw_size_count < 16) {
        logged_draw_sizes[logged_draw_size_count++] = (int)rec_vertex_count;
        logf_msg("vkskeleton: Vulkan draw size %u vertices\n",
                 (unsigned)rec_vertex_count);
    }

    p_vkCmdBindVertexBuffers(cmd_buffers[rec_slot], 0, 1, &vertex_buffer,
                             &offset);
    p_vkCmdBindIndexBuffer(cmd_buffers[rec_slot], index_buffer,
                           (VkDeviceSize)rec_slot * FRAME_INDEX_BYTES,
                           VK_INDEX_TYPE_UINT32);
    cur_eye = -1;
    cur_rect.offset.x = cur_rect.offset.y = 0;
    cur_rect.extent = swap_extent;
    replay_cmds(cmd_buffers[rec_slot]);
}

/* Record the frame's command list into the current target (cur_eye). */
static void replay_cmds(VkCommandBuffer cb)
{
    VkDeviceSize offset = (VkDeviceSize)rec_slot * FRAME_VERTEX_BYTES;
    VkPipeline pipe;
    uint32_t slot;
    float eye_pc[20];

    {
        uint32_t last_mat = ~0u;
        uint32_t ci;

        for (ci = 0; ci < rec_cmd_count; ci++) {
            const frame_cmd_t *c = &rec_cmds[ci];

            if (c->type == FCMD_CLEAR) {
                VkClearAttachment ca[2];
                VkClearRect rect = {{{0, 0}, {0, 0}}, 0, 1};
                uint32_t n = 0;
                memset(ca, 0, sizeof(ca));
                rect.rect = cur_rect;
                if ((c->mask & GL_COLOR_BUFFER_BIT) && cur_eye != VIEW_HUD) {
                    ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    ca[n].colorAttachment = 0;
                    memcpy(ca[n].clearValue.color.float32, c->color,
                           sizeof(c->color));
                    n++;
                }
                if (c->mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
                    if (c->mask & GL_DEPTH_BUFFER_BIT)
                        ca[n].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
                    if (c->mask & GL_STENCIL_BUFFER_BIT)
                        ca[n].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                    ca[n].clearValue.depthStencil.depth = 1.0f;
                    ca[n].clearValue.depthStencil.stencil = 0;
                    n++;
                }
                if (n)
                    p_vkCmdClearAttachments(cb, n, ca, 1, &rect);
                continue;
            }
            if (c->type == FCMD_COPY) {
                if (cur_eye == VIEW_HUD)
                    continue; /* mirrors are 3D */
                record_framebuffer_copy(cb, c);
                /* new render pass: rebind everything */
                p_vkCmdBindVertexBuffers(cb, 0, 1, &vertex_buffer, &offset);
                p_vkCmdBindIndexBuffer(cb, index_buffer,
                                       (VkDeviceSize)rec_slot *
                                           FRAME_INDEX_BYTES,
                                       VK_INDEX_TYPE_UINT32);
                last_mat = ~0u;
                continue;
            }
            {
                const frame_batch_t *b = &rec_batches[c->a];
                VkDescriptorSet sets[2];
                uint32_t slot2 = b->tex2;
                uint32_t pkey = b->pkey;

                /* separate HUD: 2D only in the HUD target, 3D only in eyes */
                if (rec_hud && cur_eye >= 0 &&
                    mat_is_3d(b->mat) != (cur_eye != VIEW_HUD))
                    continue;
                if (cur_eye == VIEW_HUD)
                    pkey |= PKEY_HUD_ALPHA;

                if (b->mat != last_mat) {
                    p_vkCmdPushConstants(cb, pipeline_layout,
                                         VK_SHADER_STAGE_VERTEX_BIT, 0, 80,
                                         replay_mat(b->mat, eye_pc));
                    last_mat = b->mat;
                }
                slot = b->tex;
                /* A texture deleted earlier this frame keeps its descriptor
                 * set but its image view is gone: never bind it. */
                if (slot >= (uint32_t)vk_texture_count ||
                    !vk_textures[slot].desc_set || !vk_textures[slot].view)
                    slot = 0;
                if (slot2 >= (uint32_t)vk_texture_count ||
                    !vk_textures[slot2].desc_set || !vk_textures[slot2].view)
                    slot2 = 0;
                sets[0] = vk_textures[slot].desc_set;
                sets[1] = vk_textures[slot2].desc_set;
                vk_textures[slot].last_used_serial = frame_serial + 1;
                vk_textures[slot2].last_used_serial = frame_serial + 1;
                pipe = get_pipeline(pkey);
                if (!pipe)
                    continue;
                p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                if (b->pkey & (1u << 5))
                    p_vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK,
                                               b->sref & 0xffu);
                if (b->pkey & (1u << 21))
                    p_vkCmdSetDepthBias(cb, b->bias_c, 0.0f, b->bias_s);
                p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          pipeline_layout, 0, 2, sets, 0, NULL);
                p_vkCmdDrawIndexed(cb, b->count, 1, b->first, 0, 0);
            }
        }
    }
}

static int end_frame_present(void)
{
    VkSubmitInfo si = {0};
    VkPresentInfoKHR pi = {0};
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkResult r;

    if (!frame_active)
        return 0;

    p_vkCmdEndRenderPass(cmd_buffers[rec_slot]);
    {
        int hud, eyes = record_eyes(cmd_buffers[rec_slot], &hud);
        if (vkx_frame_end && swap_can_copy)
            vkx_frame_end(cmd_buffers[rec_slot], swap_images[current_image],
                          swap_format, swap_extent, frame_fences[rec_slot],
                          rec_proj_tan, eyes ? eye_tg.image : VK_NULL_HANDLE,
                          eye_tg.extent, hud ? hud_tg.image : VK_NULL_HANDLE);
    }
    p_vkEndCommandBuffer(cmd_buffers[rec_slot]);

    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquire_sems[rec_slot];
    si.pWaitDstStageMask = &wait_stage;
    {
        static VkCommandBuffer cbs[2];
        uint32_t n = 0;
        if (rec_has_upload) /* ended by the game thread at snapshot time */
            cbs[n++] = upload_cmds[rec_slot];
        cbs[n++] = cmd_buffers[rec_slot];
        si.commandBufferCount = n;
        si.pCommandBuffers = cbs;
    }
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_sems[current_image];
    pthread_mutex_lock(&queue_mtx);
    r = p_vkQueueSubmit(queue, 1, &si, frame_fences[rec_slot]);
    pthread_mutex_unlock(&queue_mtx);
    if (r != VK_SUCCESS) {
        logf_msg("vkskeleton: QueueSubmit failed (%d)\n", (int)r);
        frame_active = 0;
        return 0;
    }
    frame_slot_serial[rec_slot] = ++frame_serial;
    /* No wait here: the next begin_frame waits only for the slot it reuses. */

    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_sems[current_image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &current_image;
    pthread_mutex_lock(&queue_mtx);
    r = p_vkQueuePresentKHR(queue, &pi);
    pthread_mutex_unlock(&queue_mtx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        swapchain_needs_rebuild = 1;
    else if (r != VK_SUCCESS)
        logf_msg("vkskeleton: QueuePresentKHR failed (%d)\n", (int)r);
    else if (!logged_first_present) {
        logged_first_present = 1;
        logmsg("vkskeleton: first Vulkan present\n");
    }

    frame_active = 0;
    return 1;
}

/* ---------- immediate-mode to triangle-list conversion ---------- */


/* Sutherland-Hodgman clipping in homogeneous clip space.  Plane 0 is the
 * GL near plane (cz + cw >= 0), plane 1 is the far plane (cw - cz >= 0). */

/* First geometry of a frame: wait until the GPU is done with this slot's
 * vertex region (frame submitted FRAMES_IN_FLIGHT frames ago). */
static int frame_slot_begin(void)
{
    if (frame_slot_ready)
        return 1;
    if (!vertex_map || !device)
        return 0;
    p_vkWaitForFences(device, 1, &frame_fences[cur_frame], VK_TRUE, UINT64_MAX);
    if (!index_map)
        return 0;
    frame_cmds = frame_cmds_store[cur_frame];
    frame_batches = frame_batches_store[cur_frame];
    frame_mats = frame_mats_store[cur_frame];
    frame_vertices = (vk_vertex_t *)((char *)vertex_map +
                                     (size_t)cur_frame * FRAME_VERTEX_BYTES);
    frame_indices = (uint32_t *)((char *)index_map +
                                 (size_t)cur_frame * FRAME_INDEX_BYTES);
    frame_vertex_count = 0;
    frame_index_count = 0;
    frame_batch_count = 0;
    frame_cmd_count = 0;
    frame_mat_count = 0;
    frame_epoch++;
    frame_slot_ready = 1;
    return 1;
}

/* Account 3 just-written vertices to the current batch. */
static void batch_add_tri(const im_vertex_t *s)
{
    frame_batch_t *b = frame_batch_count ? &frame_batches[frame_batch_count - 1]
                                         : NULL;
    if (b && b->tex == s->tex && b->tex2 == s->tex2 &&
        b->pkey == cur_draw_pkey && b->sref == cur_draw_sref &&
        b->bias_s == cur_draw_bias_s && b->bias_c == cur_draw_bias_c &&
        b->mat == cur_draw_mat &&
        b->first + b->count == frame_index_count && frame_cmd_count &&
        frame_cmds[frame_cmd_count - 1].type == FCMD_DRAW &&
        frame_cmds[frame_cmd_count - 1].a == frame_batch_count - 1) {
        b->count += 3;
    } else if (frame_batch_count < MAX_FRAME_BATCHES &&
               frame_cmd_count < MAX_FRAME_CMDS) {
        frame_cmds[frame_cmd_count].type = FCMD_DRAW;
        frame_cmds[frame_cmd_count].a = frame_batch_count;
        frame_cmd_count++;
        b = &frame_batches[frame_batch_count++];
        b->first = (uint32_t)frame_index_count;
        b->count = 3;
        b->tex = s->tex;
        b->tex2 = s->tex2;
        b->pkey = cur_draw_pkey;
        b->sref = cur_draw_sref;
        b->bias_s = cur_draw_bias_s;
        b->bias_c = cur_draw_bias_c;
        b->mat = cur_draw_mat;
    } else {
        return; /* batch table full: drop (logged as overflow) */
    }
    frame_index_count += 3;
}

/* GL clip space -> Vulkan clip space, folded into the draw's matrix:
 *   y' = -y                      (GL origin bottom-left, Vulkan top-left)
 *   z' = n*w + (f-n)*(z+w)/2     (GL clip z in [-w,w] -> glDepthRange [n,f],
 *                                 inside Vulkan's [0,w])
 * Returns the matrix index for this draw, reusing the previous one when
 * unchanged (world and studio models share one matrix in GoldSrc). */
/* GL clip space -> Vulkan: flip Y, apply glDepthRange (z' = a*z + b*w). */
static void fix_clip(const float *m, float a, float b, float *o)
{
    int c;
    for (c = 0; c < 4; c++) {
        o[c * 4 + 0] = m[c * 4 + 0];
        o[c * 4 + 1] = -m[c * 4 + 1];
        o[c * 4 + 2] = a * m[c * 4 + 2] + b * m[c * 4 + 3];
        o[c * 4 + 3] = m[c * 4 + 3];
    }
}

static uint32_t frame_matrix_for_draw(void)
{
    float o[MAT_FLOATS];
    float a = (depth_range_far - depth_range_near) * 0.5f;
    float b = (depth_range_far + depth_range_near) * 0.5f;
    int c;

    fix_clip(mat_mvp.m, a, b, o);
    memcpy(o + 20, mat_proj.m, 16 * sizeof(float));
    memcpy(o + 36, mat_model.m, 16 * sizeof(float));
    o[52] = a;
    o[53] = b;
    o[54] = o[55] = 0.0f;
    /* glClipPlane: the eye-space plane, expressed in this draw's object
     * space (plane_obj = plane_eye * ModelView), so the vertex shader can
     * use the untransformed position: gl_ClipDistance = dot(plane, pos). */
    if (clip0_enabled) {
        const float *mv = mat_model.m;
        for (c = 0; c < 4; c++)
            o[16 + c] = clip0_eye[0] * mv[c * 4 + 0] +
                        clip0_eye[1] * mv[c * 4 + 1] +
                        clip0_eye[2] * mv[c * 4 + 2] +
                        clip0_eye[3] * mv[c * 4 + 3];
    } else {
        o[16] = o[17] = o[18] = 0.0f;
        o[19] = 1.0f; /* distance 1 for every vertex: nothing clipped */
    }
    if (frame_mat_count &&
        !memcmp(frame_mats[frame_mat_count - 1], o, sizeof(o)))
        return frame_mat_count - 1;
    if (frame_mat_count >= MAX_FRAME_MATS)
        return ~0u;
    memcpy(frame_mats[frame_mat_count], o, sizeof(o));
    return frame_mat_count++;
}

/* im_vertex_t starts with exactly vk_vertex_t's fields, in the same order,
 * so a pending vertex becomes a GPU vertex with one block copy (no
 * field-by-field float loads/stores). */
_Static_assert(offsetof(im_vertex_t, x) == offsetof(vk_vertex_t, x) &&
               offsetof(im_vertex_t, u) == offsetof(vk_vertex_t, u) &&
               offsetof(im_vertex_t, r) == offsetof(vk_vertex_t, r) &&
               offsetof(im_vertex_t, alpha_ref) == offsetof(vk_vertex_t, alpha_ref) &&
               offsetof(im_vertex_t, alpha_func) == offsetof(vk_vertex_t, alpha_func) &&
               offsetof(im_vertex_t, u2) == offsetof(vk_vertex_t, u2) &&
               offsetof(im_vertex_t, v2) == offsetof(vk_vertex_t, v2) &&
               sizeof(vk_vertex_t) == offsetof(im_vertex_t, v2) + sizeof(float),
               "im_vertex_t must begin with vk_vertex_t's layout");

/* ia, ib, ic index im_vertices[].  No CPU transform or clipping: the vertex
 * shader applies the draw's matrix and the GPU clips and divides. */
static void emit_tri(size_t ia, size_t ib, size_t ic)
{
    uint32_t *dst;

    if (frame_index_count + 3 > FRAME_MAX_INDICES ||
        frame_batch_count >= MAX_FRAME_BATCHES) {
        note_frame_overflow();
        return;
    }
    /* the draw's vertices were already copied once (append_geometry) */
    dst = &frame_indices[frame_index_count];
    dst[0] = cur_draw_base + (uint32_t)ia;
    dst[1] = cur_draw_base + (uint32_t)ib;
    dst[2] = cur_draw_base + (uint32_t)ic;
    batch_add_tri(&im_vertices[ia]);
}

static void append_geometry(void)
{
    size_t i;

    if (!geometry_enabled)
        return;
    if (current_framebuffer != 0) {
        return;
    }
    if (!im_active || im_vertex_count < 3)
        return;

    if (color_mask_off && debug_enabled)
        dbg_event("draw colormask off (stencil-only) prim=0x%x n=%u st=%d "
                  "func=0x%x ref=%d", im_mode, (unsigned)im_vertex_count,
                  stencil_test_enabled, st_func, st_ref);
    if (!frame_slot_begin())
        return;
    {
        static unsigned done_xform, done_epoch;
        if (done_xform != xform_serial || done_epoch != frame_epoch ||
            cur_draw_mat == ~0u) {
            mat_mul(&mat_mvp, &mat_proj, &mat_model);
            cur_draw_mat = frame_matrix_for_draw();
            if (mat_proj.m[11] < -0.5f && mat_proj.m[0] != 0.0f &&
                mat_proj.m[5] != 0.0f) {
                /* glFrustum: tangents of the left/right/down/up planes */
                const float *m = mat_proj.m;
                last_proj_tan[0] = (m[8] - 1.0f) / m[0];
                last_proj_tan[1] = (m[8] + 1.0f) / m[0];
                last_proj_tan[2] = (m[9] - 1.0f) / m[5];
                last_proj_tan[3] = (m[9] + 1.0f) / m[5];
            }
            done_xform = xform_serial;
            done_epoch = frame_epoch;
        }
    }
    if (cur_draw_mat == ~0u) {
        note_frame_overflow();
        return;
    }
    if (frame_vertex_count + im_vertex_count > FRAME_MAX_VERTICES) {
        note_frame_overflow();
        return;
    }
    /* im_vertex_t begins with vk_vertex_t's exact layout: one block copy per
     * vertex into (write-combined) GPU-visible memory. */
    cur_draw_base = (uint32_t)frame_vertex_count;
    for (i = 0; i < im_vertex_count; i++)
        memcpy(&frame_vertices[frame_vertex_count + i], &im_vertices[i],
               sizeof(vk_vertex_t));
    frame_vertex_count += im_vertex_count;

    switch (im_mode) {
    case GL_TRIANGLES:
        for (i = 0; i + 2 < im_vertex_count; i += 3)
            emit_tri(i, i + 1,
                     i + 2);
        break;
    case GL_TRIANGLE_STRIP:
        for (i = 2; i < im_vertex_count; i++) {
            if ((i & 1) == 0)
                emit_tri(i - 2, i - 1,
                         i);
            else
                emit_tri(i - 1, i - 2,
                         i);
        }
        break;
    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        for (i = 2; i < im_vertex_count; i++)
            emit_tri(0, i - 1, i);
        break;
    case GL_QUADS:
        for (i = 0; i + 3 < im_vertex_count; i += 4) {
            emit_tri(i, i + 1,
                     i + 2);
            emit_tri(i, i + 2,
                     i + 3);
        }
        break;
    case GL_QUAD_STRIP:
        for (i = 2; i + 1 < im_vertex_count; i += 2) {
            emit_tri(i - 2, i - 1,
                     i);
            emit_tri(i - 1, i + 1,
                     i);
        }
        break;
    default:
        if (!logged_unsupported_primitive) {
            logged_unsupported_primitive = 1;
            logf_msg("vkskeleton: unsupported primitive 0x%x (skipped)\n",
                     (unsigned)im_mode);
        }
        break;
    }
}

/* Everything except colour/texcoords is fixed between glBegin and glEnd (GL
 * forbids state changes there), so it is resolved once per draw instead of
 * per vertex. */
static struct {
    uint32_t tex, tex2;
    float scale;
    int replace;
    uint8_t blend, depth;
    float alpha_ref, alpha_func;
} ds;

static void compute_draw_state(void)
{
    static unsigned done_serial;
    vk_texture_t *tex;

    if (done_serial == state_serial)
        return; /* nothing changed since the previous draw */
    done_serial = state_serial;

    ds.tex = ds.tex2 = 0;
    ds.scale = 1.0f;
    ds.replace = 0;
    if (unit_tex_enabled[0] && unit_texture[0]) {
        tex = get_texture_slot(unit_texture[0], 0);
        if (tex && tex->desc_set && tex->valid)
            ds.tex = (uint32_t)(tex - vk_textures);
        ds.replace = unit_env_mode[0] == GL_REPLACE;
        if (unit_env_mode[0] == GL_COMBINE_ENUM)
            ds.scale *= unit_rgb_scale[0];
    }
    if (unit_tex_enabled[1] && unit_texture[1]) {
        tex = get_texture_slot(unit_texture[1], 0);
        if (tex && tex->desc_set && tex->valid)
            ds.tex2 = (uint32_t)(tex - vk_textures);
        if (unit_env_mode[1] == GL_COMBINE_ENUM)
            ds.scale *= unit_rgb_scale[1];
    }
    ds.blend = (uint8_t)blend_mode_from_state();
    ds.depth = depth_test_enabled ? 1 : 0;
    alpha_params_for_state(&ds.alpha_ref, &ds.alpha_func);
    cur_draw_pkey = pipeline_key(ds.blend);
    if (cur_draw_pkey & (1u << 21)) {
        cur_draw_bias_s = poly_offset_factor;
        cur_draw_bias_c = poly_offset_units;
    } else {
        cur_draw_bias_s = cur_draw_bias_c = 0.0f;
    }
    cur_draw_sref = stencil_test_enabled ? (uint32_t)st_ref : 0u;
}

static void push_im_vertex(float x, float y, float z)
{
    im_vertex_t *v;
    uint32_t tex_slot = ds.tex;
    float scale = ds.scale;
    int replace = ds.replace;

    if (!geometry_enabled)
        return;
    if (!im_active || im_vertex_count >= IMMEDIATE_MAX_VERTICES)
        return;
    v = &im_vertices[im_vertex_count++];
    v->x = x;
    v->y = y;
    v->z = z;
    v->u = cur_u;
    v->v = cur_v;
    v->u2 = cur_u2;
    v->v2 = cur_v2;
    /* GL_REPLACE on unit 0 ignores the primary color.  RGB_SCALE of a
     * GL_COMBINE unit scales the modulated RGB; folding it into the vertex
     * color is equivalent because the shader output is a pure product. */
    v->r = (replace ? 1.0f : cur_color[0]) * scale;
    v->g = (replace ? 1.0f : cur_color[1]) * scale;
    v->b = (replace ? 1.0f : cur_color[2]) * scale;
    v->a = replace ? 1.0f : cur_color[3];
    v->tex = tex_slot;
    v->tex2 = ds.tex2;
    v->blend = ds.blend;
    v->replace = 0;
    v->depth = ds.depth;
    v->alpha_ref = ds.alpha_ref;
    v->alpha_func = ds.alpha_func;
}

/* GoldSrc's HUD, console and text rendering use interleaved client arrays:
 * 2 floats UV at offset 0, 2 floats XY at offset 8, stride 16, GL_QUADS. */
static void emit_array_geometry(GLenum mode, GLint first, GLsizei count)
{
    const unsigned char *vbase;
    const unsigned char *tbase;
    size_t vstride, tstride;
    GLint i;

    if (!geometry_enabled)
        return;
    if (current_framebuffer != 0) {
        return;
    }
    if (!vertex_array_enabled || !vertex_array_ptr ||
        vertex_array_type != GL_FLOAT || vertex_array_size < 2) {
        return;
    }
    if (first < 0 || count <= 0 ||
        (size_t)count > IMMEDIATE_MAX_VERTICES - im_vertex_count)
        return;

    vbase = (const unsigned char *)vertex_array_ptr;
    tbase = (const unsigned char *)texcoord_array_ptr;
    vstride = vertex_array_stride
                  ? (size_t)vertex_array_stride
                  : sizeof(float) * (size_t)vertex_array_size;
    tstride = texcoord_array_stride
                  ? (size_t)texcoord_array_stride
                  : sizeof(float) * (size_t)texcoord_array_size;

    im_mode = mode;
    im_vertex_count = 0;
    im_active = 1;
    compute_draw_state();

    {
        /* GL_COLOR_ARRAY: per-vertex colour; the current colour is put back
         * afterwards (GL leaves it undefined, the engine re-sets it). */
        float saved_color[4];
        const unsigned char *cbase = (const unsigned char *)color_array_ptr;
        size_t csize = color_array_type == GL_UNSIGNED_BYTE ? 1 : sizeof(float);
        size_t cstride = color_array_stride
                             ? (size_t)color_array_stride
                             : csize * (size_t)color_array_size;
        int use_color = color_array_enabled && cbase &&
                        (color_array_size == 3 || color_array_size == 4) &&
                        (color_array_type == GL_FLOAT ||
                         color_array_type == GL_UNSIGNED_BYTE);

        memcpy(saved_color, cur_color, sizeof(saved_color));
    for (i = 0; i < count; i++) {
        if (use_color) {
            const unsigned char *cp = cbase + (size_t)(first + i) * cstride;
            int c;
            for (c = 0; c < color_array_size; c++)
                cur_color[c] = color_array_type == GL_FLOAT
                                   ? ((const float *)cp)[c]
                                   : cp[c] / 255.0f;
            if (color_array_size == 3)
                cur_color[3] = 1.0f;
        }
        const float *vp = (const float *)(vbase +
                                          (size_t)(first + i) * vstride);
        const float *tp = NULL;

        if (texcoord_array_enabled && tbase &&
            texcoord_array_type == GL_FLOAT && texcoord_array_size >= 2) {
            tp = (const float *)(tbase + (size_t)(first + i) * tstride);
        }
        cur_u = tp ? tp[0] : 0.0f;
        cur_v = tp ? tp[1] : 0.0f;
        push_im_vertex(vp[0], vp[1],
                       vertex_array_size >= 3 ? vp[2] : 0.0f);
    }
        if (use_color)
            memcpy(cur_color, saved_color, sizeof(saved_color));
    }

    append_geometry();
    im_active = 0;
    im_vertex_count = 0;
}

/* ---------- GL / SDL hooks ---------- */

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    if (!real_gl_clear_color)
        real_gl_clear_color =
            (fn_gl_clear_color)next_symbol("glClearColor");
    clear_color[0] = r;
    clear_color[1] = g;
    clear_color[2] = b;
    clear_color[3] = a;
    if (real_gl_clear_color)
        real_gl_clear_color(r, g, b, a);
}

void glClear(GLbitfield mask)
{
    if (!real_gl_clear)
        real_gl_clear = (fn_gl_clear)next_symbol("glClear");
    dbg_event("Clear mask=0x%x", (unsigned)mask);
    if (enabled && (mask & GL_COLOR_BUFFER_BIT)) {
        init_vulkan_captured();
        frame_has_clear = 1;
    }
    /* The render pass already clears at frame start; later clears (mirror
     * views clear depth between passes) are replayed in order. */
    if (geometry_enabled && ready && !swapchain_dead && current_framebuffer == 0 &&
        frame_slot_ready && frame_cmd_count && frame_cmd_count < MAX_FRAME_CMDS &&
        (mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                 GL_STENCIL_BUFFER_BIT))) {
        frame_cmd_t *c = &frame_cmds[frame_cmd_count++];
        c->type = FCMD_CLEAR;
        c->mask = mask;
        memcpy(c->color, clear_color, sizeof(c->color));
    }
    /* GL's framebuffer is not shown while Vulkan presents */
    if (real_gl_clear && !(geometry_enabled && should_skip_gl_draw())) {
        gl_sync();
        real_gl_clear(mask);
    }
}

#ifndef VKSKELETON_NO_GEOMETRY_WRAPPERS
/* While Vulkan presents, sending the same draws to OpenGL only makes the GPU
 * render every frame twice (the GL image is never shown).  State calls are
 * still forwarded; draws into an FBO and frames Vulkan does not present still
 * go to GL. */
static int should_skip_gl_draw(void)
{
    return skip_gl_draws && ready && !swapchain_dead && current_framebuffer == 0;
}

/* Tracked state calls are skipped while Vulkan presents; GL is resynced by
 * gl_sync() before it really draws (fallback frames, FBO draws, copies). */
static int gl_skip_state(void)
{
    if (geometry_enabled && skip_gl_draws && ready && !swapchain_dead) {
        gl_out_of_sync = 1;
        return 1;
    }
    return 0;
}

static int tracked_cap(GLenum cap)
{
    return cap == GL_TEXTURE_2D || cap == GL_BLEND || cap == GL_DEPTH_TEST ||
           cap == 0xbc0 /* ALPHA_TEST */ || cap == 0xb90 /* STENCIL_TEST */ ||
           cap == 0x8037 /* POLYGON_OFFSET_FILL */ ||
           cap == 0x3000 /* CLIP_PLANE0 */;
}

void glBegin(GLenum mode)
{
    if (!real_gl_begin)
        real_gl_begin = (fn_gl_begin)next_symbol("glBegin");
    if (geometry_enabled) {
        im_mode = mode;
        im_vertex_count = 0;
        im_active = 1;
        compute_draw_state();
    }
    im_skip_gl = geometry_enabled && should_skip_gl_draw();
    if (real_gl_begin && !im_skip_gl) {
        gl_sync();
        real_gl_begin(mode);
    }
}

void glEnd(void)
{
    if (!real_gl_end)
        real_gl_end = (fn_gl_end)next_symbol("glEnd");
    if (real_gl_end && !im_skip_gl)
        real_gl_end();
    im_skip_gl = 0;
    if (geometry_enabled) {
        append_geometry();
        im_active = 0;
        im_vertex_count = 0;
    }
}

void glVertex2f(GLfloat x, GLfloat y)
{
    if (!real_gl_vertex2f)
        real_gl_vertex2f = (fn_gl_vertex2f)next_symbol("glVertex2f");
    push_im_vertex(x, y, 0.0f);
    if (real_gl_vertex2f && !im_skip_gl)
        real_gl_vertex2f(x, y);
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    if (!real_gl_vertex3f)
        real_gl_vertex3f = (fn_gl_vertex3f)next_symbol("glVertex3f");
    push_im_vertex(x, y, z);
    if (real_gl_vertex3f && !im_skip_gl)
        real_gl_vertex3f(x, y, z);
}

void glVertex3fv(const GLfloat *v)
{
    if (!real_gl_vertex3fv)
        real_gl_vertex3fv = (fn_gl_vertex3fv)next_symbol("glVertex3fv");
    if (v)
        push_im_vertex(v[0], v[1], v[2]);
    if (real_gl_vertex3fv && !im_skip_gl)
        real_gl_vertex3fv(v);
}

void glVertex3i(GLint x, GLint y, GLint z)
{
    if (!real_gl_vertex3i)
        real_gl_vertex3i = (fn_gl_vertex3i)next_symbol("glVertex3i");
    push_im_vertex((float)x, (float)y, (float)z);
    if (real_gl_vertex3i && !im_skip_gl)
        real_gl_vertex3i(x, y, z);
}

void glTexCoord2f(GLfloat s, GLfloat t)
{
    if (!real_gl_texcoord2f)
        real_gl_texcoord2f = (fn_gl_texcoord2f)next_symbol("glTexCoord2f");
    if (geometry_enabled) {
        cur_u = s;
        cur_v = t;
    }
    if (real_gl_texcoord2f && !im_skip_gl && !gl_skip_state())
        real_gl_texcoord2f(s, t);
}

void glColor3f(GLfloat r, GLfloat g, GLfloat b)
{
    if (!real_gl_color3f)
        real_gl_color3f = (fn_gl_color3f)next_symbol("glColor3f");
    if (geometry_enabled) {
        cur_color[0] = r;
        cur_color[1] = g;
        cur_color[2] = b;
        cur_color[3] = 1.0f;
    }
    if (real_gl_color3f && !im_skip_gl && !gl_skip_state())
        real_gl_color3f(r, g, b);
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    if (!real_gl_color4f)
        real_gl_color4f = (fn_gl_color4f)next_symbol("glColor4f");
    if (geometry_enabled) {
        cur_color[0] = r;
        cur_color[1] = g;
        cur_color[2] = b;
        cur_color[3] = a;
    }
    if (real_gl_color4f && !im_skip_gl && !gl_skip_state())
        real_gl_color4f(r, g, b, a);
}

void glColor3ubv(const GLubyte *v)
{
    if (!real_gl_color3ubv)
        real_gl_color3ubv = (fn_gl_color3ubv)next_symbol("glColor3ubv");
    if (geometry_enabled && v) {
        cur_color[0] = v[0] / 255.0f;
        cur_color[1] = v[1] / 255.0f;
        cur_color[2] = v[2] / 255.0f;
        cur_color[3] = 1.0f;
    }
    if (real_gl_color3ubv && !im_skip_gl && !gl_skip_state())
        real_gl_color3ubv(v);
}

void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    if (!real_gl_color4ub)
        real_gl_color4ub = (fn_gl_color4ub)next_symbol("glColor4ub");
    if (geometry_enabled) {
        cur_color[0] = r / 255.0f;
        cur_color[1] = g / 255.0f;
        cur_color[2] = b / 255.0f;
        cur_color[3] = a / 255.0f;
    }
    if (real_gl_color4ub && !im_skip_gl && !gl_skip_state())
        real_gl_color4ub(r, g, b, a);
}

void glColor4ubv(const GLubyte *v)
{
    if (!real_gl_color4ubv)
        real_gl_color4ubv = (fn_gl_color4ubv)next_symbol("glColor4ubv");
    if (geometry_enabled && v) {
        cur_color[0] = v[0] / 255.0f;
        cur_color[1] = v[1] / 255.0f;
        cur_color[2] = v[2] / 255.0f;
        cur_color[3] = v[3] / 255.0f;
    }
    if (real_gl_color4ubv && !im_skip_gl && !gl_skip_state())
        real_gl_color4ubv(v);
}

void glMatrixMode(GLenum mode)
{
    if (!real_gl_matrix_mode)
        real_gl_matrix_mode = (fn_gl_matrix_mode)next_symbol("glMatrixMode");
    if (geometry_enabled)
        matrix_mode = mode;
    if (real_gl_matrix_mode && !gl_skip_state())
        real_gl_matrix_mode(mode);
}

void glLoadIdentity(void)
{
    xform_serial++;
    if (!real_gl_load_identity)
        real_gl_load_identity = (fn_gl_void)next_symbol("glLoadIdentity");
    if (geometry_enabled)
        mat_identity(current_matrix());
    if (real_gl_load_identity && !gl_skip_state())
        real_gl_load_identity();
}

void glPushMatrix(void)
{
    if (!real_gl_push_matrix)
        real_gl_push_matrix = (fn_gl_void)next_symbol("glPushMatrix");
    if (geometry_enabled) {
        mat4 *stack = current_stack();
        int *sp = current_stack_ptr();
        if (*sp < 32)
            stack[(*sp)++] = *current_matrix();
    }
    if (real_gl_push_matrix && !gl_skip_state())
        real_gl_push_matrix();
}

void glPopMatrix(void)
{
    xform_serial++;
    if (!real_gl_pop_matrix)
        real_gl_pop_matrix = (fn_gl_void)next_symbol("glPopMatrix");
    if (geometry_enabled) {
        mat4 *stack = current_stack();
        int *sp = current_stack_ptr();
        if (*sp > 0)
            *current_matrix() = stack[--(*sp)];
    }
    if (real_gl_pop_matrix && !gl_skip_state())
        real_gl_pop_matrix();
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    xform_serial++;
    if (!real_gl_translatef)
        real_gl_translatef =
            (fn_gl_translate)next_symbol("glTranslatef");
    if (geometry_enabled)
        mat_translate(current_matrix(), x, y, z);
    if (real_gl_translatef && !gl_skip_state())
        real_gl_translatef(x, y, z);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    xform_serial++;
    if (!real_gl_rotatef)
        real_gl_rotatef = (fn_gl_rotate)next_symbol("glRotatef");
    if (geometry_enabled)
        mat_rotate(current_matrix(), angle, x, y, z);
    if (real_gl_rotatef && !gl_skip_state())
        real_gl_rotatef(angle, x, y, z);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    xform_serial++;
    if (!real_gl_scalef)
        real_gl_scalef = (fn_gl_translate)next_symbol("glScalef");
    if (geometry_enabled)
        mat_scale(current_matrix(), x, y, z);
    if (real_gl_scalef && !gl_skip_state())
        real_gl_scalef(x, y, z);
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble zNear, GLdouble zFar)
{
    xform_serial++;
    if (!real_gl_ortho)
        real_gl_ortho = (fn_gl_ortho)next_symbol("glOrtho");
    if (geometry_enabled)
        mat_ortho(current_matrix(), left, right, bottom, top, zNear, zFar);
    if (real_gl_ortho && !gl_skip_state())
        real_gl_ortho(left, right, bottom, top, zNear, zFar);
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble zNear, GLdouble zFar)
{
    xform_serial++;
    if (!real_gl_frustum)
        real_gl_frustum = (fn_gl_frustum)next_symbol("glFrustum");
    if (geometry_enabled)
        mat_frustum(current_matrix(), left, right, bottom, top, zNear, zFar);
    if (real_gl_frustum && !gl_skip_state())
        real_gl_frustum(left, right, bottom, top, zNear, zFar);
}

void glLoadMatrixf(const GLfloat *m)
{
    xform_serial++;
    if (!real_gl_load_matrixf)
        real_gl_load_matrixf =
            (fn_gl_load_matrix)next_symbol("glLoadMatrixf");
    if (geometry_enabled && m)
        memcpy(current_matrix()->m, m, 16 * sizeof(float));
    if (real_gl_load_matrixf && !gl_skip_state())
        real_gl_load_matrixf(m);
}

void glEnable(GLenum cap)
{
    state_serial++;
    xform_serial++; /* GL_CLIP_PLANE0 */
    if (!real_gl_enable)
        real_gl_enable = (fn_gl_enable)next_symbol("glEnable");
    if (geometry_enabled) {
        if (cap == GL_TEXTURE_2D) {
            unit_tex_enabled[active_unit] = 1;
        }
        if (cap == GL_BLEND)
            blend_enabled = 1;
        if (cap == GL_DEPTH_TEST)
            depth_test_enabled = 1;
        if (cap == 0xbc0) /* GL_ALPHA_TEST */
            alpha_test_enabled = 1;
        if (cap == 0xb90) /* GL_STENCIL_TEST */
            stencil_test_enabled = 1;
        if (cap == 0x8037) /* GL_POLYGON_OFFSET_FILL */
            polygon_offset_enabled = 1;
        if (cap == 0x3000) /* GL_CLIP_PLANE0 */
            clip0_enabled = 1;
    }
    if (real_gl_enable && !(tracked_cap(cap) && gl_skip_state()))
        real_gl_enable(cap);
}

void glDisable(GLenum cap)
{
    state_serial++;
    xform_serial++; /* GL_CLIP_PLANE0 */
    if (!real_gl_disable)
        real_gl_disable = (fn_gl_disable)next_symbol("glDisable");
    if (geometry_enabled) {
        if (cap == GL_TEXTURE_2D) {
            unit_tex_enabled[active_unit] = 0;
        }
        if (cap == GL_BLEND)
            blend_enabled = 0;
        if (cap == GL_DEPTH_TEST)
            depth_test_enabled = 0;
        if (cap == 0xbc0) /* GL_ALPHA_TEST */
            alpha_test_enabled = 0;
        if (cap == 0xb90) /* GL_STENCIL_TEST */
            stencil_test_enabled = 0;
        if (cap == 0x8037) /* GL_POLYGON_OFFSET_FILL */
            polygon_offset_enabled = 0;
        if (cap == 0x3000) /* GL_CLIP_PLANE0 */
            clip0_enabled = 0;
    }
    if (real_gl_disable && !(tracked_cap(cap) && gl_skip_state()))
        real_gl_disable(cap);
}

void glBindTexture(GLenum target, GLuint texture)
{
    state_serial++;
    if (!real_gl_bind_texture)
        real_gl_bind_texture = (fn_gl_bind_texture)next_symbol("glBindTexture");
    if (geometry_enabled && target == GL_TEXTURE_2D) {
        cur_texture = texture;
        unit_texture[active_unit] = texture;
    }
    if (real_gl_bind_texture)
        real_gl_bind_texture(target, texture);
}

void glPixelStorei(GLenum pname, GLint param)
{
    if (!real_gl_pixel_storei)
        real_gl_pixel_storei =
            (fn_gl_pixel_storei)next_symbol("glPixelStorei");
    if (geometry_enabled) {
        switch (pname) {
        case GL_UNPACK_ROW_LENGTH:
            unpack_row_length = param > 0 ? param : 0;
            break;
        case GL_UNPACK_ALIGNMENT:
            unpack_alignment = param > 0 ? param : 4;
            break;
        case GL_UNPACK_SKIP_ROWS:
            unpack_skip_rows = param > 0 ? param : 0;
            break;
        case GL_UNPACK_SKIP_PIXELS:
            unpack_skip_pixels = param > 0 ? param : 0;
            break;
        default:
            break;
        }
    }
    if (real_gl_pixel_storei)
        real_gl_pixel_storei(pname, param);
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLsizei height, GLint border, GLenum format, GLenum type,
                  const void *pixels)
{
    if (!real_gl_tex_image2d)
        real_gl_tex_image2d = (fn_gl_tex_image2d)next_symbol("glTexImage2D");
    if (real_gl_tex_image2d)
        real_gl_tex_image2d(target, level, internalformat, width, height,
                            border, format, type, pixels);


    if ((width >= 1024 && height >= 512) || !pixels ||
        type != GL_UNSIGNED_BYTE)
        dbg_event("teximage tex=%u %dx%d lvl=%d ifmt=0x%x fmt=0x%x type=0x%x "
                  "px=%d", cur_texture, width, height, level, internalformat,
                  format, type, pixels ? 1 : 0);
    /* Storage-only textures (pixels == NULL) are created whatever the
     * declared type: mirrors allocate their 1280x960 copy targets as
     * GL_RGBA / GL_BYTE with no data, then fill them with
     * glCopyTexSubImage2D.  Rejecting them left the copy with no image and
     * the mirror white. */
    if (geometry_enabled && ready && target == GL_TEXTURE_2D && level == 0 &&
        (format == GL_RGBA || format == GL_RGB) &&
        (type == GL_UNSIGNED_BYTE || !pixels) && width > 0 && height > 0) {
        vk_texture_t *tex = get_texture_slot(cur_texture, 1);
        if (tex)
            create_vk_texture_image(tex, (uint32_t)width, (uint32_t)height,
                                    pixels, format == GL_RGB ? 3 : 4);
    }
}

typedef void (*fn_gl_delete_textures)(GLsizei, const GLuint *);
static fn_gl_delete_textures real_gl_delete_textures;

/* Free the Vulkan side of deleted textures so their slots can be reused
 * (map changes delete thousands of textures). */
void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    state_serial++;
    GLsizei k;

    if (!real_gl_delete_textures)
        real_gl_delete_textures =
            (fn_gl_delete_textures)next_symbol("glDeleteTextures");
    if (geometry_enabled && textures && n > 0) {
        for (k = 0; k < n; k++) {
            GLuint id = textures[k];
            vk_texture_t *tex;
            int u;

            if (!id)
                continue;
            tex = get_texture_slot(id, 0);
            if (!tex || tex == &vk_textures[0])
                continue;
            if (device)
                destroy_texture_image(tex);
            if (id < TEXTURE_GL_ID_MAX)
                texture_slot_by_gl[id] = 0;
            tex->gl_id = 0;
            tex->valid = 0;
            if (free_texture_slot_count < MAX_VK_TEXTURES)
                free_texture_slots[free_texture_slot_count++] =
                    (int)(tex - vk_textures);
            /* a deleted texture bound on a unit reverts to 0 in GL */
            for (u = 0; u < MAX_TEX_UNITS; u++)
                if (unit_texture[u] == id)
                    unit_texture[u] = 0;
            if (cur_texture == id)
                cur_texture = 0;
        }
    }
    if (real_gl_delete_textures)
        real_gl_delete_textures(n, textures);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const void *pixels)
{
    int handled = 0;

    if (!real_gl_tex_sub_image2d)
        real_gl_tex_sub_image2d =
            (fn_gl_tex_sub_image2d)next_symbol("glTexSubImage2D");

    if (geometry_enabled && ready && target == GL_TEXTURE_2D && level == 0 &&
        (format == GL_RGBA || format == GL_RGB) && type == GL_UNSIGNED_BYTE &&
        pixels && width > 0 && height > 0 && xoffset >= 0 && yoffset >= 0) {
        vk_texture_t *tex = get_texture_slot(cur_texture, 0);
        if (tex && tex->valid &&
            xoffset + width <= (GLint)tex->width &&
            yoffset + height <= (GLint)tex->height) {
            upload_texture_pixels(tex, (uint32_t)xoffset, (uint32_t)yoffset,
                                  (uint32_t)width, (uint32_t)height, pixels,
                                  format == GL_RGB ? 3 : 4);
            handled = 1;
        }
    }

    /* While Vulkan presents, GL never samples these textures (its draws are
     * skipped), but Mesa still allocated and mapped an upload buffer per call
     * -- the per-frame lightmap updates showed up as GPU-buffer page faults,
     * ioctls and libgallium time in perf.  Forward only what Vulkan did not
     * take, or everything with VKSKELETON_GL_PASSTHROUGH=1. */
    if (real_gl_tex_sub_image2d && !(handled && should_skip_gl_draw()))
        real_gl_tex_sub_image2d(target, level, xoffset, yoffset, width, height,
                                format, type, pixels);
}

static void track_tex_parameter(GLenum target, GLenum pname, GLint param)
{
    vk_texture_t *tex;
    uint8_t new_value;
    int nearest;

    if (!geometry_enabled || target != GL_TEXTURE_2D)
        return;
    tex = get_texture_slot(cur_texture, 1);
    if (!tex)
        return;

    switch (pname) {
    case 0x2801: /* GL_TEXTURE_MIN_FILTER */
        nearest = param == 0x2600 /* GL_NEAREST */ ||
                  param == 0x2700 /* GL_NEAREST_MIPMAP_NEAREST */ ||
                  param == 0x2702 /* GL_NEAREST_MIPMAP_LINEAR */;
        new_value = nearest ? 1 : 0;
        if (tex->min_filter != new_value) {
            tex->min_filter = new_value;
            refresh_texture_descriptor(tex);
        }
        break;
    case 0x2800: /* GL_TEXTURE_MAG_FILTER */
        new_value = param == 0x2600 /* GL_NEAREST */ ? 1 : 0;
        if (tex->mag_filter != new_value) {
            tex->mag_filter = new_value;
            refresh_texture_descriptor(tex);
        }
        break;
    case 0x2802: /* GL_TEXTURE_WRAP_S */
        new_value = (param == 0x2900 /* GL_CLAMP */ ||
                     param == 0x812f /* GL_CLAMP_TO_EDGE */ ||
                     param == 0x812d /* GL_CLAMP_TO_BORDER */) ? 1 : 0;
        if (tex->wrap_s != new_value) {
            tex->wrap_s = new_value;
            refresh_texture_descriptor(tex);
        }
        break;
    case 0x2803: /* GL_TEXTURE_WRAP_T */
        new_value = (param == 0x2900 /* GL_CLAMP */ ||
                     param == 0x812f /* GL_CLAMP_TO_EDGE */ ||
                     param == 0x812d /* GL_CLAMP_TO_BORDER */) ? 1 : 0;
        if (tex->wrap_t != new_value) {
            tex->wrap_t = new_value;
            refresh_texture_descriptor(tex);
        }
        break;
    default:
        break;
    }
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    if (!real_gl_tex_parameteri)
        real_gl_tex_parameteri =
            (fn_gl_tex_parameteri)next_symbol("glTexParameteri");
    if (real_gl_tex_parameteri)
        real_gl_tex_parameteri(target, pname, param);
    track_tex_parameter(target, pname, param);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    if (!real_gl_tex_parameterf)
        real_gl_tex_parameterf =
            (fn_gl_tex_parameterf)next_symbol("glTexParameterf");
    if (real_gl_tex_parameterf)
        real_gl_tex_parameterf(target, pname, param);
    track_tex_parameter(target, pname, (GLint)param);
}

void glAlphaFunc(GLenum func, GLfloat ref)
{
    state_serial++;
    if (!real_gl_alpha_func)
        real_gl_alpha_func = (fn_gl_alpha_func)next_symbol("glAlphaFunc");
    if (geometry_enabled) {
        cur_alpha_func = func;
        cur_alpha_ref = ref;
    }
    if (real_gl_alpha_func && !gl_skip_state())
        real_gl_alpha_func(func, ref);
}

static void track_tex_env(GLenum target, GLenum pname, float param)
{
    state_serial++;
    if (!geometry_enabled || target != GL_TEXTURE_ENV)
        return;
    if (pname == GL_TEXTURE_ENV_MODE) {
        unit_env_mode[active_unit] = (GLenum)param;
    } else if (pname == GL_RGB_SCALE_ENUM) {
        unit_rgb_scale[active_unit] = param > 0.0f ? param : 1.0f;
    }
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    if (!real_gl_tex_envf)
        real_gl_tex_envf = (fn_gl_tex_envf)next_symbol("glTexEnvf");
    track_tex_env(target, pname, param);
    if (real_gl_tex_envf &&
        !(target == GL_TEXTURE_ENV &&
          (pname == GL_TEXTURE_ENV_MODE || pname == GL_RGB_SCALE_ENUM) &&
          gl_skip_state()))
        real_gl_tex_envf(target, pname, param);
}

void glTexEnvi(GLenum target, GLenum pname, GLint param)
{
    if (!real_gl_tex_envi)
        real_gl_tex_envi = (fn_gl_tex_envi)next_symbol("glTexEnvi");
    track_tex_env(target, pname, (float)param);
    if (real_gl_tex_envi &&
        !(target == GL_TEXTURE_ENV &&
          (pname == GL_TEXTURE_ENV_MODE || pname == GL_RGB_SCALE_ENUM) &&
          gl_skip_state()))
        real_gl_tex_envi(target, pname, param);
}

/* ---------- multitexture (world surfaces: unit 0 = texture, 1 = lightmap) */

static void set_active_unit(GLenum texture)
{
    int unit = (int)texture - GL_TEXTURE0_ENUM;

    if (!geometry_enabled)
        return;
    if (unit < 0 || unit >= MAX_TEX_UNITS)
        unit = 0;
    active_unit = unit;
    cur_texture = unit_texture[unit];
}

static void set_multi_texcoord(GLenum target, float s, float t)
{
    int unit = (int)target - GL_TEXTURE0_ENUM;

    if (!geometry_enabled)
        return;
    if (unit == 0) {
        cur_u = s;
        cur_v = t;
    } else if (unit == 1) {
        cur_u2 = s;
        cur_v2 = t;
    }
}

void glActiveTexture(GLenum texture)
{
    if (!real_gl_active_texture)
        real_gl_active_texture =
            (fn_gl_active_texture)next_symbol("glActiveTexture");
    set_active_unit(texture);
    if (real_gl_active_texture)
        real_gl_active_texture(texture);
}

void glActiveTextureARB(GLenum texture)
{
    if (!real_gl_active_texture_arb)
        real_gl_active_texture_arb =
            (fn_gl_active_texture)next_symbol("glActiveTextureARB");
    set_active_unit(texture);
    if (real_gl_active_texture_arb)
        real_gl_active_texture_arb(texture);
}

void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t)
{
    if (!real_gl_multi_texcoord2f)
        real_gl_multi_texcoord2f =
            (fn_gl_multi_texcoord2f)next_symbol("glMultiTexCoord2f");
    set_multi_texcoord(target, s, t);
    if (real_gl_multi_texcoord2f && !im_skip_gl && !gl_skip_state())
        real_gl_multi_texcoord2f(target, s, t);
}

void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t)
{
    if (!real_gl_multi_texcoord2f_arb)
        real_gl_multi_texcoord2f_arb =
            (fn_gl_multi_texcoord2f)next_symbol("glMultiTexCoord2fARB");
    set_multi_texcoord(target, s, t);
    if (real_gl_multi_texcoord2f_arb && !im_skip_gl && !gl_skip_state())
        real_gl_multi_texcoord2f_arb(target, s, t);
}

void glMultiTexCoord2fv(GLenum target, const GLfloat *v)
{
    if (!real_gl_multi_texcoord2fv)
        real_gl_multi_texcoord2fv =
            (fn_gl_multi_texcoord2fv)next_symbol("glMultiTexCoord2fv");
    if (v)
        set_multi_texcoord(target, v[0], v[1]);
    if (real_gl_multi_texcoord2fv && !im_skip_gl && !gl_skip_state())
        real_gl_multi_texcoord2fv(target, v);
}

void glMultiTexCoord2fvARB(GLenum target, const GLfloat *v)
{
    if (!real_gl_multi_texcoord2fv_arb)
        real_gl_multi_texcoord2fv_arb =
            (fn_gl_multi_texcoord2fv)next_symbol("glMultiTexCoord2fvARB");
    if (v)
        set_multi_texcoord(target, v[0], v[1]);
    if (real_gl_multi_texcoord2fv_arb && !im_skip_gl && !gl_skip_state())
        real_gl_multi_texcoord2fv_arb(target, v);
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    state_serial++;
    if (!real_gl_blend_func)
        real_gl_blend_func = (fn_gl_blend_func)next_symbol("glBlendFunc");
    if (geometry_enabled) {
        cur_blend_src = sfactor;
        cur_blend_dst = dfactor;
    }
    if (real_gl_blend_func && !gl_skip_state())
        real_gl_blend_func(sfactor, dfactor);
}

void glEnableClientState(GLenum cap)
{
    if (!real_gl_enable_client_state)
        real_gl_enable_client_state =
            (fn_gl_client_state)next_symbol("glEnableClientState");
    if (geometry_enabled) {
        if (cap == GL_VERTEX_ARRAY)
            vertex_array_enabled = 1;
        else if (cap == GL_TEXTURE_COORD_ARRAY)
            texcoord_array_enabled = 1;
        else if (cap == GL_COLOR_ARRAY)
            color_array_enabled = 1;
    }
    if (real_gl_enable_client_state)
        real_gl_enable_client_state(cap);
}

void glDisableClientState(GLenum cap)
{
    if (!real_gl_disable_client_state)
        real_gl_disable_client_state =
            (fn_gl_client_state)next_symbol("glDisableClientState");
    if (geometry_enabled) {
        if (cap == GL_VERTEX_ARRAY)
            vertex_array_enabled = 0;
        else if (cap == GL_TEXTURE_COORD_ARRAY)
            texcoord_array_enabled = 0;
        else if (cap == GL_COLOR_ARRAY)
            color_array_enabled = 0;
    }
    if (real_gl_disable_client_state)
        real_gl_disable_client_state(cap);
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride,
                     const void *pointer)
{
    if (!real_gl_vertex_pointer)
        real_gl_vertex_pointer =
            (fn_gl_array_pointer)next_symbol("glVertexPointer");
    if (geometry_enabled) {
        vertex_array_size = size;
        vertex_array_type = type;
        vertex_array_stride = stride;
        vertex_array_ptr = pointer;
    }
    if (real_gl_vertex_pointer)
        real_gl_vertex_pointer(size, type, stride, pointer);
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                       const void *pointer)
{
    if (!real_gl_tex_coord_pointer)
        real_gl_tex_coord_pointer =
            (fn_gl_array_pointer)next_symbol("glTexCoordPointer");
    if (geometry_enabled) {
        texcoord_array_size = size;
        texcoord_array_type = type;
        texcoord_array_stride = stride;
        texcoord_array_ptr = pointer;
    }
    if (real_gl_tex_coord_pointer)
        real_gl_tex_coord_pointer(size, type, stride, pointer);
}

typedef void (*fn_gl_depth_range)(GLclampd, GLclampd);
static fn_gl_array_pointer real_gl_color_pointer;
static fn_gl_depth_range real_gl_depth_range;

void glColorPointer(GLint size, GLenum type, GLsizei stride,
                    const GLvoid *pointer)
{
    if (!real_gl_color_pointer)
        real_gl_color_pointer =
            (fn_gl_array_pointer)next_symbol("glColorPointer");
    if (geometry_enabled) {
        color_array_size = size;
        color_array_type = type;
        color_array_stride = stride;
        color_array_ptr = pointer;
    }
    if (real_gl_color_pointer)
        real_gl_color_pointer(size, type, stride, pointer);
}

void glDepthRange(GLclampd near_val, GLclampd far_val)
{
    xform_serial++;
    if (!real_gl_depth_range)
        real_gl_depth_range = (fn_gl_depth_range)next_symbol("glDepthRange");
    if (geometry_enabled) {
        depth_range_near = near_val < 0.0 ? 0.0f : near_val > 1.0 ? 1.0f
                                                                  : (float)near_val;
        depth_range_far = far_val < 0.0 ? 0.0f : far_val > 1.0 ? 1.0f
                                                               : (float)far_val;
        { /* distinct ranges, once: the viewmodel uses a compressed one */
            static float seen[8][2];
            static int nseen;
            int i;
            for (i = 0; i < nseen; i++)
                if (seen[i][0] == depth_range_near && seen[i][1] == depth_range_far)
                    break;
            if (i == nseen && nseen < 8) {
                seen[nseen][0] = depth_range_near;
                seen[nseen++][1] = depth_range_far;
                logf_msg("vkskeleton: glDepthRange %.3f..%.3f\n",
                         (double)depth_range_near, (double)depth_range_far);
            }
        }
    }
    if (real_gl_depth_range)
        real_gl_depth_range(near_val, far_val);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (!real_gl_draw_arrays)
        real_gl_draw_arrays = (fn_gl_draw_arrays)next_symbol("glDrawArrays");
    if (geometry_enabled)
        emit_array_geometry(mode, first, count);
    if (real_gl_draw_arrays && !(geometry_enabled && should_skip_gl_draw())) {
        gl_sync();
        real_gl_draw_arrays(mode, first, count);
    }
}

void glBindFramebufferEXT(GLenum target, GLuint framebuffer)
{
    if (!real_gl_bind_framebuffer)
        real_gl_bind_framebuffer =
            (fn_gl_bind_framebuffer)next_symbol("glBindFramebufferEXT");
    if (geometry_enabled)
        current_framebuffer = framebuffer;
    dbg_event("glBindFramebufferEXT %u", (unsigned)framebuffer);
    if (real_gl_bind_framebuffer)
        real_gl_bind_framebuffer(target, framebuffer);
}


/* ---------- debug-only wrappers: how are mirrors rendered? ---------- */
typedef void (*fn_gl_fb_tex2d)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*fn_gl_fb_rb)(GLenum, GLenum, GLenum, GLuint);
typedef void (*fn_gl_copy_tex_sub2d)(GLenum, GLint, GLint, GLint, GLint, GLint,
                                     GLsizei, GLsizei);
typedef void (*fn_gl_copy_tex2d)(GLenum, GLint, GLenum, GLint, GLint, GLsizei,
                                 GLsizei, GLint);
typedef void (*fn_gl_clip_plane)(GLenum, const GLdouble *);
typedef void (*fn_gl_stencil_func)(GLenum, GLint, GLuint);
typedef void (*fn_gl_stencil_op)(GLenum, GLenum, GLenum);
typedef void (*fn_gl_color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (*fn_gl_viewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (*fn_gl_enum1)(GLenum);
typedef void (*fn_gl_bool1)(GLboolean);
static fn_gl_bind_framebuffer real_gl_bind_framebuffer_core;
static fn_gl_fb_tex2d real_gl_fb_tex2d_ext, real_gl_fb_tex2d_core;
static fn_gl_fb_rb real_gl_fb_rb_ext, real_gl_fb_rb_core;
static fn_gl_copy_tex_sub2d real_gl_copy_tex_sub2d;
static fn_gl_copy_tex2d real_gl_copy_tex2d;
static fn_gl_clip_plane real_gl_clip_plane;
static fn_gl_stencil_func real_gl_stencil_func;
static fn_gl_stencil_op real_gl_stencil_op;
static fn_gl_color_mask real_gl_color_mask;
static fn_gl_viewport real_gl_viewport;
static fn_gl_enum1 real_gl_depth_func, real_gl_cull_face;
static fn_gl_bool1 real_gl_depth_mask;

#define LAZY(ptr, type, name) \
    do { if (!(ptr)) (ptr) = (type)next_symbol(name); } while (0)

/* GLEW-resolved (glXGetProcAddress) entry points: substituted by name, real
 * pointer captured in substitute(). */
static void vkdbg_glBindFramebuffer(GLenum target, GLuint fb)
{
    if (geometry_enabled)
        current_framebuffer = fb;
    dbg_event("glBindFramebuffer %u", (unsigned)fb);
    if (real_gl_bind_framebuffer_core)
        real_gl_bind_framebuffer_core(target, fb);
}
static void vkdbg_fb_tex(const char *name, GLenum att, GLuint tex, GLint lvl)
{
    vk_texture_t *vt = get_texture_slot(tex, 0);
    dbg_event("%s att=0x%x tex=%u lvl=%d %ux%u", name, att, tex, lvl,
              vt ? vt->width : 0, vt ? vt->height : 0);
}
static void vkdbg_glFramebufferTexture2DEXT(GLenum t, GLenum att, GLenum tt,
                                            GLuint tex, GLint lvl)
{
    vkdbg_fb_tex("FbTex2DEXT", att, tex, lvl);
    if (real_gl_fb_tex2d_ext)
        real_gl_fb_tex2d_ext(t, att, tt, tex, lvl);
}
static void vkdbg_glFramebufferTexture2D(GLenum t, GLenum att, GLenum tt,
                                         GLuint tex, GLint lvl)
{
    vkdbg_fb_tex("FbTex2D", att, tex, lvl);
    if (real_gl_fb_tex2d_core)
        real_gl_fb_tex2d_core(t, att, tt, tex, lvl);
}
static void vkdbg_glFramebufferRenderbufferEXT(GLenum t, GLenum att, GLenum rt,
                                               GLuint rb)
{
    dbg_event("FbRenderbufferEXT att=0x%x rb=%u", att, rb);
    if (real_gl_fb_rb_ext)
        real_gl_fb_rb_ext(t, att, rt, rb);
}
static void vkdbg_glFramebufferRenderbuffer(GLenum t, GLenum att, GLenum rt,
                                            GLuint rb)
{
    dbg_event("FbRenderbuffer att=0x%x rb=%u", att, rb);
    if (real_gl_fb_rb_core)
        real_gl_fb_rb_core(t, att, rt, rb);
}

/* Queue a framebuffer -> texture copy at this point of the frame. */
static int queue_framebuffer_copy(GLenum target, GLint level, GLint xoff,
                                  GLint yoff, GLint x, GLint y, GLsizei w,
                                  GLsizei h)
{
    vk_texture_t *tex;
    frame_cmd_t *c;

    if (!geometry_enabled || !ready || swapchain_dead || !swap_can_copy ||
        target != GL_TEXTURE_2D || level != 0 || current_framebuffer != 0 ||
        w <= 0 || h <= 0) {
        dbg_event("copy REJECT tex=%u ready=%d dead=%d cancopy=%d tgt=0x%x "
                  "lvl=%d fb=%u", cur_texture, ready, swapchain_dead,
                  swap_can_copy, target, level, (unsigned)current_framebuffer);
        return 0;
    }
    tex = get_texture_slot(cur_texture, 0);
    if (!tex || !tex->image || !frame_slot_begin() ||
        frame_cmd_count >= MAX_FRAME_CMDS) {
        dbg_event("copy REJECT tex=%u slot=%d image=%d", cur_texture,
                  tex ? (int)(tex - vk_textures) : -1,
                  tex && tex->image ? 1 : 0);
        return 0;
    }
    tex->copy_target = 1;
    dbg_event("copy OK tex=%u slot=%d %ux%u valid=%d", cur_texture,
              (int)(tex - vk_textures), tex->width, tex->height, tex->valid);
    c = &frame_cmds[frame_cmd_count++];
    c->type = FCMD_COPY;
    c->a = (uint32_t)(tex - vk_textures);
    c->dx = xoff;
    c->dy = yoff;
    c->sx = x;
    c->sy = y;
    c->w = w;
    c->h = h;
    return 1;
}

/* Directly imported by hw.so / client.so: interposed by symbol. */
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff,
                         GLint x, GLint y, GLsizei w, GLsizei h)
{
    int handled;

    LAZY(real_gl_copy_tex_sub2d, fn_gl_copy_tex_sub2d, "glCopyTexSubImage2D");
    dbg_event("CopyTexSubImage2D tex=%u dst=%d,%d src=%d,%d %dx%d",
              cur_texture, xoff, yoff, x, y, w, h);
    handled = queue_framebuffer_copy(target, level, xoff, yoff, x, y, w, h);
    if (real_gl_copy_tex_sub2d && !(handled && should_skip_gl_draw())) {
        gl_sync();
        real_gl_copy_tex_sub2d(target, level, xoff, yoff, x, y, w, h);
    }
}
void glCopyTexImage2D(GLenum target, GLint level, GLenum ifmt, GLint x, GLint y,
                      GLsizei w, GLsizei h, GLint border)
{
    int handled = 0;

    LAZY(real_gl_copy_tex2d, fn_gl_copy_tex2d, "glCopyTexImage2D");
    dbg_event("CopyTexImage2D tex=%u src=%d,%d %dx%d", cur_texture, x, y, w, h);
    if (geometry_enabled && ready && target == GL_TEXTURE_2D && level == 0 &&
        w > 0 && h > 0) {
        /* (re)define the texture at the copied size, then copy into it */
        vk_texture_t *tex = get_texture_slot(cur_texture, 1);
        if (tex && (tex->width != (uint32_t)w || tex->height != (uint32_t)h ||
                    !tex->image))
            create_vk_texture_image(tex, (uint32_t)w, (uint32_t)h, NULL, 4);
        handled = queue_framebuffer_copy(target, level, 0, 0, x, y, w, h);
    }
    (void)ifmt;
    if (real_gl_copy_tex2d && !(handled && should_skip_gl_draw())) {
        gl_sync();
        real_gl_copy_tex2d(target, level, ifmt, x, y, w, h, border);
    }
}
void glClipPlane(GLenum plane, const GLdouble *eq)
{
    xform_serial++;
    LAZY(real_gl_clip_plane, fn_gl_clip_plane, "glClipPlane");
    dbg_event("ClipPlane 0x%x", plane);
    if (geometry_enabled && plane == 0x3000 && eq) {
        /* GL stores the plane in eye space: eq * inverse(current modelview) */
        float inv[16];
        int c;
        if (mat_invert(mat_model.m, inv)) {
            for (c = 0; c < 4; c++)
                clip0_eye[c] = (float)(eq[0] * inv[c * 4 + 0] +
                                       eq[1] * inv[c * 4 + 1] +
                                       eq[2] * inv[c * 4 + 2] +
                                       eq[3] * inv[c * 4 + 3]);
        }
    }
    if (real_gl_clip_plane && !gl_skip_state())
        real_gl_clip_plane(plane, eq);
}
void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    state_serial++;
    LAZY(real_gl_stencil_func, fn_gl_stencil_func, "glStencilFunc");
    dbg_event("StencilFunc 0x%x ref=%d mask=0x%x", func, ref, mask);
    st_func = func;
    st_ref = ref;
    st_mask = mask;
    if (real_gl_stencil_func && !gl_skip_state())
        real_gl_stencil_func(func, ref, mask);
}
void glStencilOp(GLenum sf, GLenum zf, GLenum zp)
{
    state_serial++;
    LAZY(real_gl_stencil_op, fn_gl_stencil_op, "glStencilOp");
    dbg_event("StencilOp 0x%x 0x%x 0x%x", sf, zf, zp);
    st_sfail = sf;
    st_zfail = zf;
    st_zpass = zp;
    if (real_gl_stencil_op && !gl_skip_state())
        real_gl_stencil_op(sf, zf, zp);
}
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    state_serial++;
    LAZY(real_gl_color_mask, fn_gl_color_mask, "glColorMask");
    dbg_event("ColorMask %d%d%d%d", r, g, b, a);
    color_mask_off = !(r || g || b || a);
    color_mask_rgba[0] = r;
    color_mask_rgba[1] = g;
    color_mask_rgba[2] = b;
    color_mask_rgba[3] = a;
    if (real_gl_color_mask && !gl_skip_state())
        real_gl_color_mask(r, g, b, a);
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    LAZY(real_gl_viewport, fn_gl_viewport, "glViewport");
    dbg_event("Viewport %d,%d %dx%d", x, y, w, h);
    if (real_gl_viewport)
        real_gl_viewport(x, y, w, h);
}
void glDepthFunc(GLenum func)
{
    state_serial++;
    LAZY(real_gl_depth_func, fn_gl_enum1, "glDepthFunc");
    dbg_event("DepthFunc 0x%x", func);
    if (func >= 0x200 && func <= 0x207)
        gl_depth_func = func;
    if (real_gl_depth_func && !gl_skip_state())
        real_gl_depth_func(func);
}
void glDepthMask(GLboolean flag)
{
    state_serial++;
    LAZY(real_gl_depth_mask, fn_gl_bool1, "glDepthMask");
    dbg_event("DepthMask %d", flag);
    depth_write_enabled = flag ? 1 : 0;
    if (real_gl_depth_mask && !gl_skip_state())
        real_gl_depth_mask(flag);
}
typedef void (*fn_gl_poly_offset)(GLfloat, GLfloat);
static fn_gl_poly_offset real_gl_polygon_offset;
void glPolygonOffset(GLfloat factor, GLfloat units)
{
    state_serial++;
    LAZY(real_gl_polygon_offset, fn_gl_poly_offset, "glPolygonOffset");
    dbg_event("PolygonOffset factor=%.2f units=%.2f", factor, units);
    poly_offset_factor = factor;
    poly_offset_units = units;
    if (real_gl_polygon_offset && !gl_skip_state())
        real_gl_polygon_offset(factor, units);
}


/* Push every tracked piece of state into OpenGL (see gl_skip_state). */
static void gl_resync_state(void)
{
    static const GLenum caps[6] = {GL_BLEND, GL_DEPTH_TEST, 0xbc0, 0xb90,
                                   0x8037, 0x3000};
    int on[6];
    int u, i;

    LAZY(real_gl_enable, fn_gl_enable, "glEnable");
    LAZY(real_gl_disable, fn_gl_disable, "glDisable");
    LAZY(real_gl_tex_envf, fn_gl_tex_envf, "glTexEnvf");
    LAZY(real_gl_blend_func, fn_gl_blend_func, "glBlendFunc");
    LAZY(real_gl_alpha_func, fn_gl_alpha_func, "glAlphaFunc");
    LAZY(real_gl_depth_func, fn_gl_enum1, "glDepthFunc");
    LAZY(real_gl_depth_mask, fn_gl_bool1, "glDepthMask");
    LAZY(real_gl_color_mask, fn_gl_color_mask, "glColorMask");
    LAZY(real_gl_stencil_func, fn_gl_stencil_func, "glStencilFunc");
    LAZY(real_gl_stencil_op, fn_gl_stencil_op, "glStencilOp");
    LAZY(real_gl_polygon_offset, fn_gl_poly_offset, "glPolygonOffset");
    LAZY(real_gl_matrix_mode, fn_gl_matrix_mode, "glMatrixMode");
    LAZY(real_gl_load_matrixf, fn_gl_load_matrix, "glLoadMatrixf");
    LAZY(real_gl_load_identity, fn_gl_void, "glLoadIdentity");
    LAZY(real_gl_clip_plane, fn_gl_clip_plane, "glClipPlane");
    LAZY(real_gl_color4f, fn_gl_color4f, "glColor4f");
    LAZY(real_gl_texcoord2f, fn_gl_texcoord2f, "glTexCoord2f");
    if (!real_gl_active_texture_arb && !real_gl_active_texture)
        real_gl_active_texture_arb =
            (fn_gl_active_texture)next_symbol("glActiveTextureARB");
    if (!real_gl_enable || !real_gl_disable)
        return;

    /* per texture unit: enable + env mode / RGB scale */
    for (u = 1; u >= 0; u--) {
        fn_gl_active_texture at = real_gl_active_texture_arb
                                      ? real_gl_active_texture_arb
                                      : real_gl_active_texture;
        if (at)
            at(GL_TEXTURE0_ENUM + u);
        else if (u)
            continue;
        (unit_tex_enabled[u] ? real_gl_enable : real_gl_disable)(GL_TEXTURE_2D);
        if (real_gl_tex_envf) {
            real_gl_tex_envf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
                             (float)unit_env_mode[u]);
            if (unit_env_mode[u] == GL_COMBINE_ENUM)
                real_gl_tex_envf(GL_TEXTURE_ENV, GL_RGB_SCALE_ENUM,
                                 unit_rgb_scale[u]);
        }
    }
    {
        fn_gl_active_texture at = real_gl_active_texture_arb
                                      ? real_gl_active_texture_arb
                                      : real_gl_active_texture;
        if (at)
            at(GL_TEXTURE0_ENUM + active_unit);
    }

    on[0] = blend_enabled;
    on[1] = depth_test_enabled;
    on[2] = alpha_test_enabled;
    on[3] = stencil_test_enabled;
    on[4] = polygon_offset_enabled;
    on[5] = clip0_enabled;
    for (i = 0; i < 6; i++)
        (on[i] ? real_gl_enable : real_gl_disable)(caps[i]);

    if (real_gl_blend_func)
        real_gl_blend_func(cur_blend_src, cur_blend_dst);
    if (real_gl_alpha_func)
        real_gl_alpha_func(cur_alpha_func, cur_alpha_ref);
    if (real_gl_depth_func)
        real_gl_depth_func(gl_depth_func);
    if (real_gl_depth_mask)
        real_gl_depth_mask((GLboolean)depth_write_enabled);
    if (real_gl_color_mask)
        real_gl_color_mask(color_mask_rgba[0], color_mask_rgba[1],
                           color_mask_rgba[2], color_mask_rgba[3]);
    if (real_gl_stencil_func)
        real_gl_stencil_func(st_func, st_ref, st_mask);
    if (real_gl_stencil_op)
        real_gl_stencil_op(st_sfail, st_zfail, st_zpass);
    if (real_gl_polygon_offset)
        real_gl_polygon_offset(poly_offset_factor, poly_offset_units);

    if (real_gl_matrix_mode && real_gl_load_matrixf) {
        real_gl_matrix_mode(GL_PROJECTION);
        real_gl_load_matrixf(mat_proj.m);
        real_gl_matrix_mode(GL_MODELVIEW);
        /* clip plane is given in eye space: load it under identity */
        if (real_gl_load_identity && real_gl_clip_plane) {
            GLdouble eq[4];
            real_gl_load_identity();
            for (i = 0; i < 4; i++)
                eq[i] = clip0_eye[i];
            real_gl_clip_plane(0x3000, eq);
        }
        real_gl_load_matrixf(mat_model.m);
        real_gl_matrix_mode(matrix_mode);
    }
    if (real_gl_color4f)
        real_gl_color4f(cur_color[0], cur_color[1], cur_color[2], cur_color[3]);
    if (real_gl_texcoord2f)
        real_gl_texcoord2f(cur_u, cur_v);
}

static void gl_sync(void)
{
    if (!gl_out_of_sync)
        return;
    gl_out_of_sync = 0;
    gl_resync_state();
}

void glCullFace(GLenum mode)
{
    LAZY(real_gl_cull_face, fn_gl_enum1, "glCullFace");
    dbg_event("CullFace 0x%x", mode);
    if (real_gl_cull_face)
        real_gl_cull_face(mode);
}

#endif /* VKSKELETON_NO_GEOMETRY_WRAPPERS */


/* Render side of one frame: everything comes from the rec_* snapshot. */
static int run_render_job(void)
{
    if (begin_frame()) {
        record_frame_vertices();
        if (end_frame_present())
            return 1;
    }
    /* not presented: still execute the slot's texture uploads */
    if (rec_has_upload) {
        VkSubmitInfo si = {0};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &upload_cmds[rec_slot];
        pthread_mutex_lock(&queue_mtx);
        p_vkResetFences(device, 1, &upload_fence);
        if (p_vkQueueSubmit(queue, 1, &si, upload_fence) == VK_SUCCESS)
            p_vkWaitForFences(device, 1, &upload_fence, VK_TRUE, UINT64_MAX);
        pthread_mutex_unlock(&queue_mtx);
    }
    return 0;
}

static void *render_thread_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&rt_mtx);
    for (;;) {
        while (!rt_job_pending)
            pthread_cond_wait(&rt_job_cv, &rt_mtx);
        rt_job_pending = 0;
        rt_busy = 1;
        pthread_mutex_unlock(&rt_mtx);
        run_render_job();
        pthread_mutex_lock(&rt_mtx);
        rt_busy = 0;
        pthread_cond_broadcast(&rt_done_cv);
    }
    return NULL;
}

static void render_thread_wait_idle(void)
{
    if (!render_thread_started)
        return;
    pthread_mutex_lock(&rt_mtx);
    while (rt_job_pending || rt_busy)
        pthread_cond_wait(&rt_done_cv, &rt_mtx);
    pthread_mutex_unlock(&rt_mtx);
}

/* Game side: snapshot the finished frame, hand it to the render side (in
 * place, or to the render thread) and move on to the next slot.  Returns 1
 * if the frame was taken over by Vulkan. */
static int submit_frame(void)
{
    int presented = 1;

    if (render_thread_enabled && !render_thread_started) {
        if (pthread_create(&render_thread, NULL, render_thread_main, NULL) == 0) {
            render_thread_started = 1;
            logmsg("vkskeleton: render thread started\n");
        } else {
            render_thread_enabled = 0;
            logmsg("vkskeleton: render thread failed to start; synchronous\n");
        }
    }
    if (render_thread_started)
        render_thread_wait_idle(); /* one job in flight at a time */

    if (upload_recording) {
        p_vkEndCommandBuffer(upload_cmds[cur_frame]);
        upload_recording = 0;
        rec_has_upload = 1;
    } else {
        rec_has_upload = 0;
    }
    rec_slot = cur_frame;
    rec_cmds = frame_cmds;
    rec_cmd_count = frame_slot_ready ? frame_cmd_count : 0;
    rec_batches = frame_batches;
    rec_mats = frame_mats;
    rec_vertex_count = frame_vertex_count;
    memcpy(rec_clear_color, clear_color, sizeof(rec_clear_color));
    memcpy(rec_proj_tan, last_proj_tan, sizeof(rec_proj_tan));
    rec_eye_count = vkx_eyes ? vkx_eyes(&rec_eye_extent, rec_eyes) : 0;
    rec_hud = rec_eye_count && vkx_hud && vkx_hud();
    rec_vm_valid = rec_eye_count && vkx_viewmodel && vkx_viewmodel(rec_vm);
    if (rec_eye_count < 0 || rec_eye_count > 2 || !rec_eye_extent.width ||
        !rec_eye_extent.height || rec_eye_extent.width > 8192 ||
        rec_eye_extent.height > 8192)
        rec_eye_count = 0;

    if (render_thread_started) {
        pthread_mutex_lock(&rt_mtx);
        rt_job_pending = 1;
        pthread_cond_signal(&rt_job_cv);
        pthread_mutex_unlock(&rt_mtx);
    } else {
        presented = run_render_job();
    }

    cur_frame = (cur_frame + 1) % FRAMES_IN_FLIGHT;
    frame_slot_ready = 0;
    frame_batch_count = 0;
    frame_cmd_count = 0;
    frame_mat_count = 0;
    frame_epoch++;
    frame_vertex_count = 0;
    frame_index_count = 0;
    frame_has_clear = 0;
    return presented;
}

void SDL_GL_SwapWindow(void *window)
{
    if (debug_enabled)
        dbg.frames++;
    if (!enabled || !ready) {
        if (!real_sdl_swap_window)
            real_sdl_swap_window =
                (fn_sdl_swap_window)next_symbol("SDL_GL_SwapWindow");
        if (real_sdl_swap_window)
            real_sdl_swap_window(window);
        return;
    }

    if ((frame_has_clear || frame_vertex_count) && submit_frame())
        return;
    /* Frame not presented by Vulkan (e.g. minimized/resizing): drop it, but
     * still execute its texture uploads. */
    if (upload_recording)
        flush_uploads_sync();
    frame_vertex_count = 0;
    frame_index_count = 0;
    frame_batch_count = 0;
    frame_cmd_count = 0;
    frame_mat_count = 0;
    frame_epoch++;
    frame_has_clear = 0;

    if (!real_sdl_swap_window)
        real_sdl_swap_window =
            (fn_sdl_swap_window)next_symbol("SDL_GL_SwapWindow");
    if (real_sdl_swap_window)
        real_sdl_swap_window(window);
}


/* ---------- freecam (I) ----------
 * The engine asks client.so for the view every frame via V_CalcRefdef
 * (looked up with dlsym, which we intercept).  While freecam is on, the
 * wrapper lets the game compute its view, then replaces the camera origin
 * and angles: the engine renders *and culls* from the freecam position.
 * Controls: I toggle, W/A/S/D fly, Space/Ctrl up/down, Shift faster.
 * Looking around uses the game's own view angles (the mouse keeps turning
 * the player normally): reading SDL relative motion ourselves picked up the
 * game's cursor re-centring warps and drove the camera to the pitch limit.
 * Keys are read with SDL_GetKeyboardState each frame (the SDL_PollEvent
 * hook alone did not see F5 in-game), and the player is frozen by clearing
 * movement in CL_CreateMove's usercmd rather than by swallowing keys.
 * ref_params_t (HL SDK): vieworg[3] @0, viewangles[3] @12, forward/right/up
 * @24/36/48, frametime @60. */
typedef void (*fn_v_calcrefdef)(void *);
typedef const uint8_t *(*fn_sdl_get_keyboard_state)(int *);
typedef void (*fn_cl_createmove)(float, void *, int);
static fn_sdl_get_keyboard_state real_sdl_get_keyboard_state;
static fn_cl_createmove real_cl_createmove;
static int fc_prev_toggle;
static fn_v_calcrefdef real_v_calcrefdef;

/* freecam toggle key: I (SDL_SCANCODE_I = 12) */
enum { SC_A = 4, SC_D = 7, SC_I = 12, SC_S = 22, SC_W = 26, SC_SPACE = 44,
       SC_LCTRL = 224, SC_LSHIFT = 225 };

static int fc_active, fc_init;  /* freecam on / needs start position */
static float fc_org[3], fc_ang[3];
static int fc_key[256];


/* Returns 1 if the event is consumed by the freecam. */

/* usercmd_t (HL SDK) on i386: lerp_msec@0 msec@2 viewangles@4
 * forwardmove@16 sidemove@20 upmove@24 lightlevel@28 buttons@30. */
#define UCMD_FORWARDMOVE 16
#define UCMD_SIDEMOVE 20
#define UCMD_UPMOVE 24
#define UCMD_BUTTONS 30
#define IN_MOVE_BITS ((1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 9) | \
                      (1 << 10)) /* JUMP DUCK FORWARD BACK MOVELEFT MOVERIGHT */

static void vk_CL_CreateMove(float frametime, void *cmd, int active)
{
    static int logged;

    if (real_cl_createmove)
        real_cl_createmove(frametime, cmd, active);
    if (vkx_create_move && cmd)
        vkx_create_move(frametime, cmd, active);
    if (!logged) {
        logged = 1;
        logmsg("vkskeleton: freecam: CL_CreateMove hook active\n");
    }
    if (fc_active && cmd) {
        unsigned char *c = (unsigned char *)cmd;
        float zero = 0.0f;
        uint16_t buttons;
        memcpy(c + UCMD_FORWARDMOVE, &zero, 4);
        memcpy(c + UCMD_SIDEMOVE, &zero, 4);
        memcpy(c + UCMD_UPMOVE, &zero, 4);
        memcpy(&buttons, c + UCMD_BUTTONS, 2);
        buttons &= (uint16_t)~IN_MOVE_BITS;
        memcpy(c + UCMD_BUTTONS, &buttons, 2);
    }
}

/* Key state for the freecam.  A key counts as down if SDL or X11 says so:
 * the game's libSDL2 is (most likely) sdl2-compat on SDL3, and neither the
 * SDL_PollEvent hook nor SDL_GetKeyboardState saw key presses in-game, so
 * the X11 keymap of the game's own display is queried as well. */
static int x11_key_down(const char *keys, KeySym sym)
{
    static KeyCode cache[8];
    static KeySym cache_sym[8];
    static int ncache;
    KeyCode kc = 0;
    int i;

    for (i = 0; i < ncache; i++)
        if (cache_sym[i] == sym)
            kc = cache[i];
    if (!kc && vk_dpy) {
        kc = XKeysymToKeycode(vk_dpy, sym);
        if (ncache < 8) {
            cache_sym[ncache] = sym;
            cache[ncache++] = kc;
        }
    }
    return kc && (keys[kc >> 3] & (1 << (kc & 7)));
}

static void freecam_poll_keys(void)
{
    static const int scs[8] = {SC_I, SC_W, SC_A, SC_S, SC_D, SC_SPACE,
                               SC_LCTRL, SC_LSHIFT};
    static const KeySym syms[8] = {XK_i, XK_w, XK_a, XK_s, XK_d, XK_space,
                                   XK_Control_L, XK_Shift_L};
    static int logged_sdl, logged_x11;
    const uint8_t *ks = NULL;
    char xkeys[32];
    int have_x11 = 0, k, toggle;

    if (!real_sdl_get_keyboard_state)
        real_sdl_get_keyboard_state = (fn_sdl_get_keyboard_state)next_symbol(
            "SDL_GetKeyboardState");
    if (real_sdl_get_keyboard_state)
        ks = real_sdl_get_keyboard_state(NULL);
    if (vk_dpy) {
        memset(xkeys, 0, sizeof(xkeys));
        XQueryKeymap(vk_dpy, xkeys);
        have_x11 = 1;
    }
    for (k = 0; k < 8; k++) {
        int sdl_down = ks && ks[scs[k]];
        int x11_down = have_x11 && x11_key_down(xkeys, syms[k]);
        if (sdl_down && !logged_sdl) {
            logged_sdl = 1;
            logmsg("vkskeleton: freecam: key press seen via SDL\n");
        }
        if (x11_down && !logged_x11) {
            logged_x11 = 1;
            logmsg("vkskeleton: freecam: key press seen via X11\n");
        }
        fc_key[scs[k]] = sdl_down || x11_down;
    }
    toggle = fc_key[SC_I];
    if (toggle && !fc_prev_toggle) {
        fc_active = !fc_active;
        fc_init = fc_active;
        logf_msg("vkskeleton: freecam %s\n", fc_active ? "on" : "off");
    }
    fc_prev_toggle = toggle;
}

static void vk_V_CalcRefdef(void *pparams)
{
    float *p = (float *)pparams;
    float dt, sp, cp, sy, cy, f[3], r[3], u[3], speed, mv[3];
    int i;

    if (real_v_calcrefdef)
        real_v_calcrefdef(pparams);
    {
        static int logged;
        if (!logged) {
            logged = 1;
            logmsg("vkskeleton: freecam: V_CalcRefdef hook active\n");
        }
    }
    if (vkx_view && p)
        vkx_view(p);
    freecam_poll_keys();
    if (!fc_active || !p)
        return;
    if (fc_init) { /* start where the player's view is */
        memcpy(fc_org, p, sizeof(fc_org));
        fc_init = 0;
    }
    /* look direction = the game's current view angles (mouse turns the
     * player as usual); no roll */
    fc_ang[0] = p[3];
    fc_ang[1] = p[4];
    fc_ang[2] = 0.0f;

    sp = sinf(fc_ang[0] * (float)M_PI / 180.0f);
    cp = cosf(fc_ang[0] * (float)M_PI / 180.0f);
    sy = sinf(fc_ang[1] * (float)M_PI / 180.0f);
    cy = cosf(fc_ang[1] * (float)M_PI / 180.0f);
    f[0] = cp * cy; f[1] = cp * sy; f[2] = -sp;  /* AngleVectors, roll 0 */
    r[0] = sy;      r[1] = -cy;     r[2] = 0.0f;
    u[0] = sp * cy; u[1] = sp * sy; u[2] = cp;

    dt = p[15];
    if (!(dt > 0.0f) || dt > 0.1f)
        dt = 0.016f;
    speed = (fc_key[SC_LSHIFT] ? 1200.0f : 400.0f) * dt;
    for (i = 0; i < 3; i++) {
        mv[i] = 0.0f;
        if (fc_key[SC_W]) mv[i] += f[i];
        if (fc_key[SC_S]) mv[i] -= f[i];
        if (fc_key[SC_D]) mv[i] += r[i];
        if (fc_key[SC_A]) mv[i] -= r[i];
    }
    if (fc_key[SC_SPACE]) mv[2] += 1.0f;
    if (fc_key[SC_LCTRL]) mv[2] -= 1.0f;
    for (i = 0; i < 3; i++)
        fc_org[i] += mv[i] * speed;

    memcpy(p, fc_org, sizeof(fc_org));       /* vieworg */
    memcpy(p + 3, fc_ang, sizeof(fc_ang));   /* viewangles */
    memcpy(p + 6, f, sizeof(f));             /* forward */
    memcpy(p + 9, r, sizeof(r));             /* right */
    memcpy(p + 12, u, sizeof(u));            /* up */
}

static void *substitute(const char *symbol, void *result)
{
    if (!enabled || !symbol || !result)
        return result;

    if (!strcmp(symbol, "glClear")) return (void *)glClear;
    if (!strcmp(symbol, "glClearColor")) return (void *)glClearColor;
    if (!strcmp(symbol, "SDL_GL_SwapWindow")) return (void *)SDL_GL_SwapWindow;
    if (!strcmp(symbol, "V_CalcRefdef")) { /* client.so view hook: freecam */
        if (!real_v_calcrefdef)
            real_v_calcrefdef = (fn_v_calcrefdef)result;
        return (void *)vk_V_CalcRefdef;
    }
    if (!strcmp(symbol, "CL_CreateMove")) { /* freeze player in freecam */
        if (!real_cl_createmove)
            real_cl_createmove = (fn_cl_createmove)result;
        return (void *)vk_CL_CreateMove;
    }
    if (vkx_client_symbol) {
        Dl_info di; /* other client.so exports: offered to the module */
        size_t n;
        if (dladdr(result, &di) && di.dli_fname &&
            (n = strlen(di.dli_fname)) >= 9 &&
            !strcmp(di.dli_fname + n - 9, "client.so")) {
            void *w = vkx_client_symbol(symbol, result);
            if (w)
                return w;
        }
    }
    if (!geometry_enabled)
        return result;

#ifndef VKSKELETON_NO_GEOMETRY_WRAPPERS
    if (!strcmp(symbol, "glBegin")) return (void *)glBegin;
    if (!strcmp(symbol, "glEnd")) return (void *)glEnd;
    if (!strcmp(symbol, "glVertex2f")) return (void *)glVertex2f;
    if (!strcmp(symbol, "glVertex3f")) return (void *)glVertex3f;
    if (!strcmp(symbol, "glVertex3fv")) return (void *)glVertex3fv;
    if (!strcmp(symbol, "glVertex3i")) return (void *)glVertex3i;
    if (!strcmp(symbol, "glTexCoord2f")) return (void *)glTexCoord2f;
    if (!strcmp(symbol, "glColor3f")) return (void *)glColor3f;
    if (!strcmp(symbol, "glColor4f")) return (void *)glColor4f;
    if (!strcmp(symbol, "glColor3ubv")) return (void *)glColor3ubv;
    if (!strcmp(symbol, "glColor4ub")) return (void *)glColor4ub;
    if (!strcmp(symbol, "glColor4ubv")) return (void *)glColor4ubv;
    if (!strcmp(symbol, "glMatrixMode")) return (void *)glMatrixMode;
    if (!strcmp(symbol, "glLoadIdentity")) return (void *)glLoadIdentity;
    if (!strcmp(symbol, "glPushMatrix")) return (void *)glPushMatrix;
    if (!strcmp(symbol, "glPopMatrix")) return (void *)glPopMatrix;
    if (!strcmp(symbol, "glTranslatef")) return (void *)glTranslatef;
    if (!strcmp(symbol, "glRotatef")) return (void *)glRotatef;
    if (!strcmp(symbol, "glScalef")) return (void *)glScalef;
    if (!strcmp(symbol, "glOrtho")) return (void *)glOrtho;
    if (!strcmp(symbol, "glFrustum")) return (void *)glFrustum;
    if (!strcmp(symbol, "glLoadMatrixf")) return (void *)glLoadMatrixf;
    if (!strcmp(symbol, "glEnable")) return (void *)glEnable;
    if (!strcmp(symbol, "glDisable")) return (void *)glDisable;
    if (!strcmp(symbol, "glBindTexture")) return (void *)glBindTexture;
    if (!strcmp(symbol, "glPixelStorei")) return (void *)glPixelStorei;
    if (!strcmp(symbol, "glTexImage2D")) return (void *)glTexImage2D;
    if (!strcmp(symbol, "glTexSubImage2D")) return (void *)glTexSubImage2D;
    if (!strcmp(symbol, "glDeleteTextures")) return (void *)glDeleteTextures;
    if (!strcmp(symbol, "glCopyTexSubImage2D")) return (void *)glCopyTexSubImage2D;
    if (!strcmp(symbol, "glCopyTexImage2D")) return (void *)glCopyTexImage2D;
    if (!strcmp(symbol, "glClipPlane")) return (void *)glClipPlane;
    if (!strcmp(symbol, "glStencilFunc")) return (void *)glStencilFunc;
    if (!strcmp(symbol, "glStencilOp")) return (void *)glStencilOp;
    if (!strcmp(symbol, "glColorMask")) return (void *)glColorMask;
    if (!strcmp(symbol, "glViewport")) return (void *)glViewport;
    if (!strcmp(symbol, "glDepthFunc")) return (void *)glDepthFunc;
    if (!strcmp(symbol, "glDepthMask")) return (void *)glDepthMask;
    if (!strcmp(symbol, "glCullFace")) return (void *)glCullFace;
    if (!strcmp(symbol, "glPolygonOffset")) return (void *)glPolygonOffset;
    if (!strcmp(symbol, "glTexParameteri")) return (void *)glTexParameteri;
    if (!strcmp(symbol, "glTexParameterf")) return (void *)glTexParameterf;
    if (!strcmp(symbol, "glAlphaFunc")) return (void *)glAlphaFunc;
    if (!strcmp(symbol, "glTexEnvf")) return (void *)glTexEnvf;
    if (!strcmp(symbol, "glBlendFunc")) return (void *)glBlendFunc;
    if (!strcmp(symbol, "glEnableClientState")) return (void *)glEnableClientState;
    if (!strcmp(symbol, "glDisableClientState")) return (void *)glDisableClientState;
    if (!strcmp(symbol, "glVertexPointer")) return (void *)glVertexPointer;
    if (!strcmp(symbol, "glTexCoordPointer")) return (void *)glTexCoordPointer;
    if (!strcmp(symbol, "glDrawArrays")) return (void *)glDrawArrays;
    if (!strcmp(symbol, "glBindFramebufferEXT")) return (void *)glBindFramebufferEXT;
    if (!strcmp(symbol, "glTexEnvi")) return (void *)glTexEnvi;
    if (!strcmp(symbol, "glColorPointer")) return (void *)glColorPointer;
    if (!strcmp(symbol, "glDepthRange")) return (void *)glDepthRange;
#define SUBST_REAL(name, real, type)                       \
    if (!strcmp(symbol, #name)) {                          \
        if (!(real))                                       \
            (real) = (type)result;                         \
        return (void *)name;                               \
    }
    SUBST_REAL(glActiveTexture, real_gl_active_texture, fn_gl_active_texture)
#define SUBST_AS(sym, fn, real, type)                      \
    if (!strcmp(symbol, sym)) {                            \
        if (!(real))                                       \
            (real) = (type)result;                         \
        return (void *)fn;                                 \
    }
    SUBST_AS("glBindFramebuffer", vkdbg_glBindFramebuffer,
             real_gl_bind_framebuffer_core, fn_gl_bind_framebuffer)
    SUBST_AS("glFramebufferTexture2DEXT", vkdbg_glFramebufferTexture2DEXT,
             real_gl_fb_tex2d_ext, fn_gl_fb_tex2d)
    SUBST_AS("glFramebufferTexture2D", vkdbg_glFramebufferTexture2D,
             real_gl_fb_tex2d_core, fn_gl_fb_tex2d)
    SUBST_AS("glFramebufferRenderbufferEXT", vkdbg_glFramebufferRenderbufferEXT,
             real_gl_fb_rb_ext, fn_gl_fb_rb)
    SUBST_AS("glFramebufferRenderbuffer", vkdbg_glFramebufferRenderbuffer,
             real_gl_fb_rb_core, fn_gl_fb_rb)
#undef SUBST_AS
    SUBST_REAL(glActiveTextureARB, real_gl_active_texture_arb, fn_gl_active_texture)
    SUBST_REAL(glMultiTexCoord2f, real_gl_multi_texcoord2f, fn_gl_multi_texcoord2f)
    SUBST_REAL(glMultiTexCoord2fARB, real_gl_multi_texcoord2f_arb, fn_gl_multi_texcoord2f)
    SUBST_REAL(glMultiTexCoord2fv, real_gl_multi_texcoord2fv, fn_gl_multi_texcoord2fv)
    SUBST_REAL(glMultiTexCoord2fvARB, real_gl_multi_texcoord2fv_arb, fn_gl_multi_texcoord2fv)
#undef SUBST_REAL
#endif /* VKSKELETON_NO_GEOMETRY_WRAPPERS */
    return result;
}

void *dlsym(void *handle, const char *symbol)
{
    void *result;

    if (!real_dlsym)
        return NULL;
    result = real_dlsym(handle, symbol);
    return substitute(symbol, result);
}

__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name)
{
    void *result;

    if (!real_getproc_arb)
        real_getproc_arb =
            (getproc_t)real_dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    result = real_getproc_arb ? (void *)real_getproc_arb(name) : NULL;
    return (__GLXextFuncPtr)substitute((const char *)name, result);
}

__GLXextFuncPtr glXGetProcAddress(const GLubyte *name)
{
    void *result;

    if (!real_getproc)
        real_getproc = (getproc_t)real_dlsym(RTLD_NEXT, "glXGetProcAddress");
    result = real_getproc ? (void *)real_getproc(name) : NULL;
    return (__GLXextFuncPtr)substitute((const char *)name, result);
}

__attribute__((constructor)) static void vkskeleton_init(void)
{
    const char *e;
    const char *log_path;

    log_path = getenv("VKSKELETON_LOG");
    if (!log_path || !*log_path)
        log_path = "/tmp/vkskeleton.log";
    e = getenv("VKSKELETON_LOG_PID");
    if (e && *e && strcmp(e, "0"))
        snprintf(log_path_buf, sizeof(log_path_buf), "%s.%d", log_path,
                 (int)getpid());
    else
        snprintf(log_path_buf, sizeof(log_path_buf), "%s", log_path);
    log_fd = open(log_path_buf, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    real_dlsym = (dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
    if (!real_dlsym)
        real_dlsym = (dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");

    e = getenv("VKSKELETON_ENABLE");
    enabled = e && *e && strcmp(e, "0");
    e = getenv("VKSKELETON_RENDER_THREAD");
    render_thread_enabled = e && *e && strcmp(e, "0");
    e = getenv("VKSKELETON_GL_PASSTHROUGH");
    skip_gl_draws = !(e && *e && strcmp(e, "0"));
    e = getenv("VKSKELETON_DEBUG");
    debug_enabled = enabled && e && *e && strcmp(e, "0");
    e = getenv("VKSKELETON_GEOMETRY");
    geometry_enabled = enabled && e && *e && strcmp(e, "0");
    mat_identity(&mat_model);
    mat_identity(&mat_proj);
    mat_identity(&mat_texture);
    mat_identity(&mat_color);
    mat_identity(&mat_mvp);

    if (!enabled)
        logmsg("vkskeleton: loaded (disabled; set VKSKELETON_ENABLE=1)\n");
    else
        logf_msg("vkskeleton: loaded (enabled, geometry=%d)\n",
                 geometry_enabled);
}
