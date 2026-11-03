/* inFlux software OpenGL. See gl.h. Compiled once for the kernel, once for user space. */
#include "gl.h"
#include "gl_port.h"   /* GL_MALLOC, GL_FREE, GL_MEMSET, GL_MEMCPY, GL_STRLEN, GL_GET_CURRENT, GL_SET_CURRENT, GL_FORGET_CONTEXT */

#define MAX_STACK 32
#define MAX_TEX_STACK 8
#define MAX_TEXTURES 256
#define MAX_TEX_SIZE 1024
#define MAX_LEVELS 11          /* 1024x1024 down to 1x1 */
#define MAX_LIGHTS 8
#define MAX_LISTS 65536
#define MAX_LIST_NESTING 64
#define INITIAL_VERTS 3072     /* divisible by 1, 2, 3 and 4 so independent primitives flush on a boundary */
#define MAX_VERTS (1 << 22)
#define MAX_FB_DIM 8192        /* also GL_MAX_VIEWPORT_DIMS */
#define MAX_IMAGE_PIXELS ((size_t)MAX_FB_DIM * MAX_FB_DIM)   /* largest client image converted in one go (gluBuild2DMipmaps input) */
#define CLIP_W_EPS 1e-4f

/* ---- math ---- */
float glm_sqrtf(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
float glm_sinf(float x) {
    /* range reduce to [-pi, pi] then minimax polynomial */
    const float twopi = 2.0f * GLM_PI;
    x = x - twopi * (float)(long)(x / twopi);
    if (x > GLM_PI) x -= twopi; else if (x < -GLM_PI) x += twopi;
    if (x > GLM_PI / 2) x = GLM_PI - x; else if (x < -GLM_PI / 2) x = -GLM_PI - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}
float glm_cosf(float x) { return glm_sinf(x + GLM_PI / 2); }
float glm_tanf(float x) { return glm_sinf(x) / glm_cosf(x); }
static float fabsf_(float x) { return x < 0 ? -x : x; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static long long clampll(long long v, long long lo, long long hi) { return v < lo ? lo : v > hi ? hi : v; }
static float fastfloor(float v) { int i = (int)v; return (float)(v < i ? i - 1 : i); }
static float log2f_(float x) {
    if (!(x > 0)) return -128;
    union { float f; uint32_t u; } v = { x };
    int e = (int)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x7FFFFF) | 0x3F800000;                  /* mantissa in [1, 2) */
    float t = (v.f - 1) / (v.f + 1), t2 = t * t;           /* log2(m) = 2/ln2 * atanh(t) */
    return e + 2.885390082f * t * (1 + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 * (1.0f / 9)))));
}
static float exp2f_(float x) {
    if (x < -126) return 0;
    if (x > 127) x = 127;
    float fl = fastfloor(x), y = (x - fl) * 0.693147181f;
    float p = 1 + y * (1 + y * (0.5f + y * (1.0f / 6 + y * (1.0f / 24 + y * (1.0f / 120 + y * (1.0f / 720))))));
    union { float f; uint32_t u; } v; v.u = (uint32_t)((int)fl + 127) << 23;
    return p * v.f;
}
float glm_powf(float b, float e) { if (e == 0) return 1; if (!(b > 0)) return 0; return exp2f_(e * log2f_(b)); }
static float expf_(float x) { return exp2f_(x * 1.442695041f); }

typedef float mat4[16];   /* column-major like OpenGL */

static void mat_identity(mat4 m) { GL_MEMSET(m, 0, sizeof(mat4)); m[0] = m[5] = m[10] = m[15] = 1; }
static int mat_is_identity(const mat4 m) { for (int i = 0; i < 16; i++) if (m[i] != ((i % 5) == 0 ? 1.0f : 0.0f)) return 0; return 1; }
static void mat_mul(mat4 out, const mat4 a, const mat4 b) {   /* out = a * b */
    mat4 r;
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
        r[c * 4 + rr] = s;
    }
    GL_MEMCPY(out, r, sizeof r);
}
static void mat_vec(const mat4 m, const float *v, float *out) {
    float r[4];
    for (int i = 0; i < 4; i++) r[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
    GL_MEMCPY(out, r, sizeof r);
}
static void normalize3(float *v) { float l = glm_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cp4(float *d, const float *s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
static void set4(float *d, float a, float b, float c, float e) { d[0] = a; d[1] = b; d[2] = c; d[3] = e; }
static void *gl_grow(void *p, size_t old_bytes, size_t new_bytes) {
    void *n = GL_MALLOC(new_bytes);
    if (!n) return 0;
    if (p) { GL_MEMCPY(n, p, old_bytes); GL_FREE(p); }
    return n;
}

/* ---- context ---- */
struct vertex { float pos[4], color[4], normal[3], tex[4]; };
struct cvert { float clip[4], col[4], bcol[4], tex[4], fog; };          /* clip space, lit; all floats so it can be lerped as an array */
struct tvertex { float x, y, z, iw; float c[4], bc[4]; float s, t, q, fog; };   /* window space (y down) */
struct level { int w, h; uint32_t *px; };
struct texture { struct level lv[MAX_LEVELS]; int used, nlevels, mip_complete; GLenum base, internal, mag, min, wrap_s, wrap_t; };
struct material { float ambient[4], diffuse[4], specular[4], emission[4], shininess; };
struct light { int enabled; float ambient[4], diffuse[4], specular[4], pos[4], dir[3], spot_dir[3], spot_exp, spot_cutoff, spot_cos, att[3]; };
struct array { int enabled, size; GLenum type; GLsizei stride; const GLvoid *ptr; };
union word { float f; uint32_t u; };
struct dlist { union word *w; int len, cap, used; void **blobs; int nblobs; };

struct gl_context {
    int width, height;
    uint32_t *color;
    float *depth;
    /* per-fragment state */
    float clear_color[4], clear_depth;
    int vp_x, vp_y, vp_w, vp_h; float depth_near, depth_far;
    int sc_x, sc_y, sc_w, sc_h;
    int cx0, cy0, cx1, cy1;            /* clip rectangle in buffer pixels (y down): viewport ∩ scissor ∩ buffer */
    GLenum depth_func; int depth_write;
    GLenum alpha_func; float alpha_ref;
    GLenum blend_src, blend_dst;
    uint32_t color_mask;               /* ARGB bits glColorMask lets through */
    /* enables */
    int depth_test, cull, lighting, blend, tex2d, color_material, alpha_test, scissor_test, fog, normalize, rescale_normal;
    int offset_fill, offset_line, offset_point, accepted_caps[8];
    /* rasterisation */
    GLenum cull_face, front_face, shade, poly_front, poly_back;
    float point_size, line_width, offset_factor, offset_units;
    /* transform */
    GLenum matrix_mode;
    mat4 modelview[MAX_STACK], projection[MAX_STACK], texmat[MAX_TEX_STACK]; int mv_top, pr_top, tx_top;
    /* lighting and fog */
    struct light lights[MAX_LIGHTS]; float ambient_model[4]; int local_viewer, two_side;
    struct material mat[2];            /* front, back */
    GLenum cm_face, cm_mode;
    GLenum fog_mode; float fog_density, fog_start, fog_end, fog_color[4];
    /* immediate mode */
    int in_begin; GLenum prim_mode; struct vertex cur; struct vertex *verts; struct cvert *cverts; int nverts, cap_verts;
    struct array va_vertex, va_color, va_normal, va_tex;
    /* textures and pixel storage */
    struct texture *textures; GLuint bound_tex; GLenum tex_env; float tex_env_color[4];
    int unpack_align, unpack_row_len, unpack_skip_rows, unpack_skip_px, pack_align, pack_row_len, pack_skip_rows, pack_skip_px;
    /* display lists */
    struct dlist *lists; int nlists; struct dlist building; GLuint compiling, list_base; GLenum list_mode; int list_depth;
    GLenum error;
    char renderer[128];                /* glGetString(GL_RENDERER) */
};

/* The current context lives in the port: per thread in the kernel (demo windows render concurrently), a static in user space */
#define CUR() ((GLcontext *)GL_GET_CURRENT())
#define DEFAULT_RENDERER "inFlux Software Rasterizer"

static void tex_defaults(struct texture *t) {
    GL_MEMSET(t, 0, sizeof *t);
    t->used = 1; t->base = t->internal = GL_RGBA;
    t->mag = GL_LINEAR; t->min = GL_NEAREST_MIPMAP_LINEAR; t->wrap_s = t->wrap_t = GL_REPEAT;
}
static void tex_free_levels(struct texture *t) { for (int i = 0; i < MAX_LEVELS; i++) if (t->lv[i].px) { GL_FREE(t->lv[i].px); t->lv[i].px = 0; } }
static void material_defaults(struct material *m) {
    set4(m->ambient, 0.2f, 0.2f, 0.2f, 1); set4(m->diffuse, 0.8f, 0.8f, 0.8f, 1);
    set4(m->specular, 0, 0, 0, 1); set4(m->emission, 0, 0, 0, 1); m->shininess = 0;
}

GLcontext *glxCreateContext(int w, int h) {
    if (w <= 0 || h <= 0 || w > MAX_FB_DIM || h > MAX_FB_DIM) return 0;   /* also keeps w * h * 4 far from overflow */
    GLcontext *c = GL_MALLOC(sizeof *c);
    if (!c) return 0;
    GL_MEMSET(c, 0, sizeof *c);
    c->width = w; c->height = h;
    GL_MEMCPY(c->renderer, DEFAULT_RENDERER, sizeof DEFAULT_RENDERER);
    c->color = GL_MALLOC((size_t)w * h * 4);
    c->depth = GL_MALLOC((size_t)w * h * sizeof(float));
    c->cap_verts = INITIAL_VERTS;
    c->verts = GL_MALLOC(c->cap_verts * sizeof(struct vertex));
    c->cverts = GL_MALLOC(c->cap_verts * sizeof(struct cvert));
    c->textures = GL_MALLOC(MAX_TEXTURES * sizeof(struct texture));
    /* zero the textures before any failure exit: glxDestroyContext frees every texture's levels */
    if (c->textures) GL_MEMSET(c->textures, 0, MAX_TEXTURES * sizeof(struct texture));
    if (!c->color || !c->depth || !c->verts || !c->cverts || !c->textures) { glxDestroyContext(c); return 0; }
    GL_MEMSET(c->color, 0, (size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) c->depth[i] = 1.0f;
    c->clear_depth = 1.0f;
    c->vp_w = w; c->vp_h = h; c->sc_w = w; c->sc_h = h; c->depth_far = 1;
    c->depth_func = GL_LESS; c->depth_write = 1; c->alpha_func = GL_ALWAYS;
    c->blend_src = GL_ONE; c->blend_dst = GL_ZERO; c->color_mask = 0xFFFFFFFFu;
    c->cull_face = GL_BACK; c->front_face = GL_CCW; c->shade = GL_SMOOTH; c->poly_front = c->poly_back = GL_FILL;
    c->point_size = 1.0f; c->line_width = 1.0f;
    c->matrix_mode = GL_MODELVIEW;
    mat_identity(c->modelview[0]); mat_identity(c->projection[0]); mat_identity(c->texmat[0]);
    set4(c->cur.color, 1, 1, 1, 1); c->cur.normal[2] = 1; c->cur.tex[3] = 1; c->cur.pos[3] = 1;
    for (int i = 0; i < MAX_LIGHTS; i++) {
        struct light *l = &c->lights[i];
        float on = i == 0 ? 1.0f : 0.0f;
        set4(l->ambient, 0, 0, 0, 1); set4(l->diffuse, on, on, on, 1); set4(l->specular, on, on, on, 1);
        set4(l->pos, 0, 0, 1, 0); l->dir[2] = 1; l->spot_dir[2] = -1;
        l->spot_cutoff = 180; l->spot_cos = -1; l->att[0] = 1;
    }
    set4(c->ambient_model, 0.2f, 0.2f, 0.2f, 1);
    material_defaults(&c->mat[0]); material_defaults(&c->mat[1]);
    c->cm_face = GL_FRONT_AND_BACK; c->cm_mode = GL_AMBIENT_AND_DIFFUSE;
    c->fog_mode = GL_EXP; c->fog_density = 1; c->fog_end = 1;
    c->tex_env = GL_MODULATE;
    c->unpack_align = c->pack_align = 4;
    tex_defaults(&c->textures[0]);
    return c;
}
static void dlist_free(struct dlist *l) {
    if (l->w) GL_FREE(l->w);
    for (int i = 0; i < l->nblobs; i++) if (l->blobs[i]) GL_FREE(l->blobs[i]);
    if (l->blobs) GL_FREE(l->blobs);
    GL_MEMSET(l, 0, sizeof *l);
}
void glxDestroyContext(GLcontext *c) {
    if (!c) return;
    if (c->textures) { for (int i = 0; i < MAX_TEXTURES; i++) tex_free_levels(&c->textures[i]); GL_FREE(c->textures); }
    if (c->lists) { for (int i = 0; i < c->nlists; i++) dlist_free(&c->lists[i]); GL_FREE(c->lists); }
    dlist_free(&c->building);
    if (c->color) GL_FREE(c->color);
    if (c->depth) GL_FREE(c->depth);
    if (c->verts) GL_FREE(c->verts);
    if (c->cverts) GL_FREE(c->cverts);
    GL_FORGET_CONTEXT(c);
    GL_FREE(c);
}
int glxMakeCurrent(GLcontext *c) { return GL_SET_CURRENT(c); }
uint32_t *glxGetPixels(GLcontext *c) { return c ? c->color : 0; }
int glxGetWidth(GLcontext *c) { return c ? c->width : 0; }
int glxGetHeight(GLcontext *c) { return c ? c->height : 0; }
void glxSetRendererName(GLcontext *c, const char *n) { if (!c || !n) return; size_t l = GL_STRLEN(n); if (l > sizeof c->renderer - 1) l = sizeof c->renderer - 1; GL_MEMCPY(c->renderer, n, l); c->renderer[l] = 0; }

static void set_error(GLenum e) { GLcontext *c = CUR(); if (c && c->error == GL_NO_ERROR) c->error = e; }
#define CTX(ret) GLcontext *c = CUR(); if (!c) return ret

/* ---- display list recording ----
 * A list is a stream of words: a header (opcode | argument count << 8) followed
 * by the arguments as floats. Enums, names and masks are small enough to be
 * exact in a float. Pixel data lives in the list's blob array. Every command
 * that can be compiled records itself at the top of its public entry point;
 * convenience variants (glColor3f, glTranslatef...) funnel into one recorded
 * command. */
enum {
    OP_CLEAR = 1, OP_CLEAR_COLOR, OP_CLEAR_DEPTH, OP_VIEWPORT, OP_SCISSOR, OP_DEPTH_RANGE, OP_ENABLE, OP_DISABLE,
    OP_DEPTH_FUNC, OP_DEPTH_MASK, OP_COLOR_MASK, OP_ALPHA_FUNC, OP_CULL_FACE, OP_FRONT_FACE, OP_SHADE_MODEL,
    OP_BLEND_FUNC, OP_POLYGON_MODE, OP_POLYGON_OFFSET, OP_POINT_SIZE, OP_LINE_WIDTH,
    OP_MATRIX_MODE, OP_LOAD_IDENTITY, OP_LOAD_MATRIX, OP_MULT_MATRIX, OP_PUSH_MATRIX, OP_POP_MATRIX,
    OP_LIGHT, OP_LIGHT_MODEL, OP_MATERIAL, OP_COLOR_MATERIAL, OP_FOG,
    OP_BEGIN, OP_END, OP_VERTEX, OP_COLOR, OP_NORMAL, OP_TEXCOORD,
    OP_BIND_TEXTURE, OP_TEX_IMAGE, OP_TEX_SUB_IMAGE, OP_TEX_PARAMETER, OP_TEX_ENV, OP_CALL_LIST, OP_CALL_LIST_OFFSET,
};
static void record(GLcontext *c, int op, int n, const float *a) {
    struct dlist *l = &c->building;
    if (l->len + n + 1 > l->cap) {
        int ncap = l->cap ? l->cap * 2 : 256;
        while (ncap < l->len + n + 1) ncap *= 2;
        union word *w = gl_grow(l->w, (size_t)l->len * sizeof *w, (size_t)ncap * sizeof *w);
        if (!w) { set_error(GL_OUT_OF_MEMORY); return; }
        l->w = w; l->cap = ncap;
    }
    l->w[l->len++].u = (uint32_t)op | (uint32_t)n << 8;
    for (int i = 0; i < n; i++) l->w[l->len++].f = a[i];
}
static int record_blob(GLcontext *c, void *p) {
    struct dlist *l = &c->building;
    void **b = gl_grow(l->blobs, (size_t)l->nblobs * sizeof *b, (size_t)(l->nblobs + 1) * sizeof *b);
    if (!b) { GL_FREE(p); set_error(GL_OUT_OF_MEMORY); return -1; }
    l->blobs = b; l->blobs[l->nblobs] = p;
    return l->nblobs++;
}
#define RECORD_ARRAY(op, arr, n) do { if (c->compiling) { record(c, op, n, arr); if (c->list_mode == GL_COMPILE) return; } } while (0)
#define RECORD(op, ...) do { if (c->compiling) { const float a_[] = { __VA_ARGS__ }; RECORD_ARRAY(op, a_, (int)(sizeof a_ / sizeof a_[0])); } } while (0)

/* number of floats a vector parameter carries */
static int param_count(GLenum p) {
    switch (p) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: case GL_EMISSION: case GL_AMBIENT_AND_DIFFUSE:
    case GL_LIGHT_MODEL_AMBIENT: case GL_FOG_COLOR: case GL_TEXTURE_ENV_COLOR: case GL_TEXTURE_BORDER_COLOR: return 4;
    case GL_SPOT_DIRECTION: return 3;
    }
    return 1;
}

/* ---- per-fragment state ---- */
static uint32_t pack_color(float r, float g, float b, float a) {
    return ((uint32_t)(clamp01(a) * 255.0f + 0.5f) << 24) | ((uint32_t)(clamp01(r) * 255.0f + 0.5f) << 16) | ((uint32_t)(clamp01(g) * 255.0f + 0.5f) << 8) | (uint32_t)(clamp01(b) * 255.0f + 0.5f);
}
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { CTX(); RECORD(OP_CLEAR_COLOR, r, g, b, a); set4(c->clear_color, clamp01(r), clamp01(g), clamp01(b), clamp01(a)); }
void glClearDepth(GLclampd d) { CTX(); RECORD(OP_CLEAR_DEPTH, (float)d); c->clear_depth = clamp01((float)d); }
void glClear(GLbitfield mask) {
    CTX(); RECORD(OP_CLEAR, (float)mask);
    if (c->in_begin) { set_error(GL_INVALID_OPERATION); return; }
    int x0 = 0, y0 = 0, x1 = c->width, y1 = c->height;
    if (c->scissor_test) {   /* 64-bit: the box is any int rectangle */
        long long sx1 = (long long)c->sc_x + c->sc_w, sy0 = (long long)c->height - c->sc_y - c->sc_h, sy1 = (long long)c->height - c->sc_y;
        x0 = (int)clampll(c->sc_x, 0, x1); x1 = (int)clampll(sx1, 0, x1); y0 = (int)clampll(sy0, 0, y1); y1 = (int)clampll(sy1, 0, y1);
    }
    if (x0 >= x1 || y0 >= y1) return;
    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t col = pack_color(c->clear_color[0], c->clear_color[1], c->clear_color[2], c->clear_color[3]), m = c->color_mask;
        for (int y = y0; y < y1; y++) {
            uint32_t *row = c->color + (size_t)y * c->width;
            if (m == 0xFFFFFFFFu) for (int x = x0; x < x1; x++) row[x] = col;
            else for (int x = x0; x < x1; x++) row[x] = (col & m) | (row[x] & ~m);
        }
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && c->depth_write)
        for (int y = y0; y < y1; y++) { float *row = c->depth + (size_t)y * c->width; for (int x = x0; x < x1; x++) row[x] = c->clear_depth; }
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { CTX(); RECORD(OP_VIEWPORT, (float)x, (float)y, (float)w, (float)h); if (w < 0 || h < 0) { set_error(GL_INVALID_VALUE); return; } c->vp_x = x; c->vp_y = y; c->vp_w = w < MAX_FB_DIM ? w : MAX_FB_DIM; c->vp_h = h < MAX_FB_DIM ? h : MAX_FB_DIM; }   /* sizes clamp to GL_MAX_VIEWPORT_DIMS */
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { CTX(); RECORD(OP_SCISSOR, (float)x, (float)y, (float)w, (float)h); if (w < 0 || h < 0) { set_error(GL_INVALID_VALUE); return; } c->sc_x = x; c->sc_y = y; c->sc_w = w; c->sc_h = h; }
void glDepthRange(GLclampd n, GLclampd f) { CTX(); RECORD(OP_DEPTH_RANGE, (float)n, (float)f); c->depth_near = clamp01((float)n); c->depth_far = clamp01((float)f); }
static void update_clip(GLcontext *c) {   /* 64-bit: viewport and scissor origins are any int */
    long long x0 = c->vp_x, y0 = (long long)c->height - c->vp_y - c->vp_h, x1 = (long long)c->vp_x + c->vp_w, y1 = (long long)c->height - c->vp_y;
    if (c->scissor_test) {
        long long sx1 = (long long)c->sc_x + c->sc_w, sy0 = (long long)c->height - c->sc_y - c->sc_h, sy1 = (long long)c->height - c->sc_y;
        if (c->sc_x > x0) x0 = c->sc_x; if (sx1 < x1) x1 = sx1; if (sy0 > y0) y0 = sy0; if (sy1 < y1) y1 = sy1;
    }
    c->cx0 = (int)clampll(x0, 0, c->width); c->cy0 = (int)clampll(y0, 0, c->height);
    c->cx1 = (int)clampll(x1, 0, c->width); c->cy1 = (int)clampll(y1, 0, c->height);
}

static void apply_color_material(GLcontext *c, const float *col);
static int *cap_ptr(GLcontext *c, GLenum cap) {
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS) return &c->lights[cap - GL_LIGHT0].enabled;
    switch (cap) {
    case GL_DEPTH_TEST: return &c->depth_test; case GL_CULL_FACE: return &c->cull; case GL_LIGHTING: return &c->lighting;
    case GL_BLEND: return &c->blend; case GL_TEXTURE_2D: return &c->tex2d; case GL_COLOR_MATERIAL: return &c->color_material;
    case GL_ALPHA_TEST: return &c->alpha_test; case GL_SCISSOR_TEST: return &c->scissor_test; case GL_FOG: return &c->fog;
    case GL_NORMALIZE: return &c->normalize; case GL_RESCALE_NORMAL: return &c->rescale_normal;
    case GL_POLYGON_OFFSET_FILL: return &c->offset_fill; case GL_POLYGON_OFFSET_LINE: return &c->offset_line; case GL_POLYGON_OFFSET_POINT: return &c->offset_point;
    /* accepted so programs that toggle them keep working; they have no effect */
    case GL_DITHER: return &c->accepted_caps[0]; case GL_POINT_SMOOTH: return &c->accepted_caps[1]; case GL_LINE_SMOOTH: return &c->accepted_caps[2];
    case GL_POLYGON_SMOOTH: return &c->accepted_caps[3]; case GL_LINE_STIPPLE: return &c->accepted_caps[4]; case GL_TEXTURE_1D: return &c->accepted_caps[5];
    }
    return 0;
}
static struct array *client_array(GLcontext *c, GLenum a) {
    switch (a) { case GL_VERTEX_ARRAY: return &c->va_vertex; case GL_COLOR_ARRAY: return &c->va_color; case GL_NORMAL_ARRAY: return &c->va_normal; case GL_TEXTURE_COORD_ARRAY: return &c->va_tex; }
    return 0;
}
void glEnable(GLenum cap) {
    CTX(); RECORD(OP_ENABLE, (float)cap);
    int *p = cap_ptr(c, cap); if (!p) { set_error(GL_INVALID_ENUM); return; }
    *p = 1;
    if (cap == GL_COLOR_MATERIAL) apply_color_material(c, c->cur.color);
}
void glDisable(GLenum cap) { CTX(); RECORD(OP_DISABLE, (float)cap); int *p = cap_ptr(c, cap); if (p) *p = 0; else set_error(GL_INVALID_ENUM); }
GLboolean glIsEnabled(GLenum cap) {
    CTX(0);
    int *p = cap_ptr(c, cap); if (p) return (GLboolean)(*p != 0);
    struct array *a = client_array(c, cap); if (a) return (GLboolean)(a->enabled != 0);
    set_error(GL_INVALID_ENUM); return 0;
}
static int valid_func(GLenum f) { return f >= GL_NEVER && f <= GL_ALWAYS; }
void glDepthFunc(GLenum f) { CTX(); RECORD(OP_DEPTH_FUNC, (float)f); if (!valid_func(f)) { set_error(GL_INVALID_ENUM); return; } c->depth_func = f; }
void glDepthMask(GLboolean f) { CTX(); RECORD(OP_DEPTH_MASK, (float)f); c->depth_write = f != 0; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    CTX(); RECORD(OP_COLOR_MASK, (float)r, (float)g, (float)b, (float)a);
    c->color_mask = (a ? 0xFF000000u : 0) | (r ? 0x00FF0000u : 0) | (g ? 0x0000FF00u : 0) | (b ? 0x000000FFu : 0);
}
void glAlphaFunc(GLenum f, GLclampf ref) { CTX(); RECORD(OP_ALPHA_FUNC, (float)f, ref); if (!valid_func(f)) { set_error(GL_INVALID_ENUM); return; } c->alpha_func = f; c->alpha_ref = clamp01(ref); }
void glCullFace(GLenum m) { CTX(); RECORD(OP_CULL_FACE, (float)m); if (m != GL_FRONT && m != GL_BACK && m != GL_FRONT_AND_BACK) { set_error(GL_INVALID_ENUM); return; } c->cull_face = m; }
void glFrontFace(GLenum m) { CTX(); RECORD(OP_FRONT_FACE, (float)m); if (m != GL_CW && m != GL_CCW) { set_error(GL_INVALID_ENUM); return; } c->front_face = m; }
void glShadeModel(GLenum m) { CTX(); RECORD(OP_SHADE_MODEL, (float)m); if (m != GL_FLAT && m != GL_SMOOTH) { set_error(GL_INVALID_ENUM); return; } c->shade = m; }
static int valid_blend(GLenum f, int src) {
    switch (f) {
    case GL_ZERO: case GL_ONE: case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR: case GL_DST_COLOR: case GL_ONE_MINUS_DST_COLOR:
    case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA: case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA: return 1;
    case GL_SRC_ALPHA_SATURATE: return src;
    }
    return 0;
}
void glBlendFunc(GLenum s, GLenum d) { CTX(); RECORD(OP_BLEND_FUNC, (float)s, (float)d); if (!valid_blend(s, 1) || !valid_blend(d, 0)) { set_error(GL_INVALID_ENUM); return; } c->blend_src = s; c->blend_dst = d; }
void glPolygonMode(GLenum face, GLenum mode) {
    CTX(); RECORD(OP_POLYGON_MODE, (float)face, (float)mode);
    if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) || (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL)) { set_error(GL_INVALID_ENUM); return; }
    if (face != GL_BACK) c->poly_front = mode;
    if (face != GL_FRONT) c->poly_back = mode;
}
void glPolygonOffset(GLfloat factor, GLfloat units) { CTX(); RECORD(OP_POLYGON_OFFSET, factor, units); c->offset_factor = factor; c->offset_units = units; }
void glPointSize(GLfloat s) { CTX(); RECORD(OP_POINT_SIZE, s); if (s <= 0) { set_error(GL_INVALID_VALUE); return; } c->point_size = s; }
void glLineWidth(GLfloat w) { CTX(); RECORD(OP_LINE_WIDTH, w); if (w <= 0) { set_error(GL_INVALID_VALUE); return; } c->line_width = w; }
void glHint(GLenum target, GLenum mode) {
    CTX();
    if (target < GL_PERSPECTIVE_CORRECTION_HINT || target > GL_FOG_HINT || mode < GL_DONT_CARE || mode > GL_NICEST) set_error(GL_INVALID_ENUM);
}
void glFlush(void) {}
void glFinish(void) {}
GLenum glGetError(void) { CTX(GL_NO_ERROR); GLenum e = c->error; c->error = GL_NO_ERROR; return e; }
const GLubyte *glGetString(GLenum name) {
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"inFlux";
    case GL_RENDERER: { GLcontext *c = CUR(); return (const GLubyte *)(c ? c->renderer : DEFAULT_RENDERER); }
    case GL_VERSION: return (const GLubyte *)"1.1 inFlux software";
    case GL_EXTENSIONS: return (const GLubyte *)"GL_EXT_bgra GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp GL_ARB_texture_mirrored_repeat GL_EXT_texture_env_add GL_EXT_rescale_normal GL_EXT_draw_range_elements";
    }
    set_error(GL_INVALID_ENUM);
    return 0;
}

/* ---- state queries ---- */
static float *top_matrix(GLcontext *c) {
    return c->matrix_mode == GL_PROJECTION ? c->projection[c->pr_top] : c->matrix_mode == GL_TEXTURE ? c->texmat[c->tx_top] : c->modelview[c->mv_top];
}
static int get_state(GLcontext *c, GLenum p, float *v) {
    int *cap = cap_ptr(c, p); if (cap) { v[0] = (float)*cap; return 1; }
    struct array *a = client_array(c, p); if (a) { v[0] = (float)a->enabled; return 1; }
    switch (p) {
    case GL_CURRENT_COLOR: cp4(v, c->cur.color); return 4;
    case GL_CURRENT_NORMAL: v[0] = c->cur.normal[0]; v[1] = c->cur.normal[1]; v[2] = c->cur.normal[2]; return 3;
    case GL_CURRENT_TEXTURE_COORDS: cp4(v, c->cur.tex); return 4;
    case GL_POINT_SIZE: v[0] = c->point_size; return 1;
    case GL_LINE_WIDTH: v[0] = c->line_width; return 1;
    case GL_POLYGON_MODE: v[0] = (float)c->poly_front; v[1] = (float)c->poly_back; return 2;
    case GL_CULL_FACE_MODE: v[0] = (float)c->cull_face; return 1;
    case GL_FRONT_FACE: v[0] = (float)c->front_face; return 1;
    case GL_SHADE_MODEL: v[0] = (float)c->shade; return 1;
    case GL_LIGHT_MODEL_AMBIENT: cp4(v, c->ambient_model); return 4;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: v[0] = (float)c->local_viewer; return 1;
    case GL_LIGHT_MODEL_TWO_SIDE: v[0] = (float)c->two_side; return 1;
    case GL_COLOR_MATERIAL_FACE: v[0] = (float)c->cm_face; return 1;
    case GL_COLOR_MATERIAL_PARAMETER: v[0] = (float)c->cm_mode; return 1;
    case GL_FOG_MODE: v[0] = (float)c->fog_mode; return 1;
    case GL_FOG_DENSITY: v[0] = c->fog_density; return 1;
    case GL_FOG_START: v[0] = c->fog_start; return 1;
    case GL_FOG_END: v[0] = c->fog_end; return 1;
    case GL_FOG_COLOR: cp4(v, c->fog_color); return 4;
    case GL_DEPTH_RANGE: v[0] = c->depth_near; v[1] = c->depth_far; return 2;
    case GL_DEPTH_WRITEMASK: v[0] = (float)c->depth_write; return 1;
    case GL_DEPTH_CLEAR_VALUE: v[0] = c->clear_depth; return 1;
    case GL_DEPTH_FUNC: v[0] = (float)c->depth_func; return 1;
    case GL_ALPHA_TEST_FUNC: v[0] = (float)c->alpha_func; return 1;
    case GL_ALPHA_TEST_REF: v[0] = c->alpha_ref; return 1;
    case GL_BLEND_SRC: v[0] = (float)c->blend_src; return 1;
    case GL_BLEND_DST: v[0] = (float)c->blend_dst; return 1;
    case GL_COLOR_CLEAR_VALUE: cp4(v, c->clear_color); return 4;
    case GL_COLOR_WRITEMASK: set4(v, (c->color_mask >> 16) & 1, (c->color_mask >> 8) & 1, c->color_mask & 1, c->color_mask >> 31); return 4;
    case GL_POLYGON_OFFSET_FACTOR: v[0] = c->offset_factor; return 1;
    case GL_POLYGON_OFFSET_UNITS: v[0] = c->offset_units; return 1;
    case GL_MATRIX_MODE: v[0] = (float)c->matrix_mode; return 1;
    case GL_VIEWPORT: set4(v, (float)c->vp_x, (float)c->vp_y, (float)c->vp_w, (float)c->vp_h); return 4;
    case GL_SCISSOR_BOX: set4(v, (float)c->sc_x, (float)c->sc_y, (float)c->sc_w, (float)c->sc_h); return 4;
    case GL_MODELVIEW_STACK_DEPTH: v[0] = (float)(c->mv_top + 1); return 1;
    case GL_PROJECTION_STACK_DEPTH: v[0] = (float)(c->pr_top + 1); return 1;
    case GL_TEXTURE_STACK_DEPTH: v[0] = (float)(c->tx_top + 1); return 1;
    case GL_MODELVIEW_MATRIX: GL_MEMCPY(v, c->modelview[c->mv_top], sizeof(mat4)); return 16;
    case GL_PROJECTION_MATRIX: GL_MEMCPY(v, c->projection[c->pr_top], sizeof(mat4)); return 16;
    case GL_TEXTURE_MATRIX: GL_MEMCPY(v, c->texmat[c->tx_top], sizeof(mat4)); return 16;
    case GL_MAX_LIGHTS: v[0] = MAX_LIGHTS; return 1;
    case GL_MAX_TEXTURE_SIZE: v[0] = MAX_TEX_SIZE; return 1;
    case GL_MAX_MODELVIEW_STACK_DEPTH: case GL_MAX_PROJECTION_STACK_DEPTH: v[0] = MAX_STACK; return 1;
    case GL_MAX_TEXTURE_STACK_DEPTH: v[0] = MAX_TEX_STACK; return 1;
    case GL_MAX_LIST_NESTING: v[0] = MAX_LIST_NESTING; return 1;
    case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = MAX_FB_DIM; return 2;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: v[0] = 8; return 1;
    case GL_DEPTH_BITS: v[0] = 24; return 1;
    case GL_STENCIL_BITS: v[0] = 0; return 1;
    case GL_TEXTURE_BINDING_2D: v[0] = (float)c->bound_tex; return 1;
    case GL_UNPACK_ALIGNMENT: v[0] = (float)c->unpack_align; return 1;
    case GL_UNPACK_ROW_LENGTH: v[0] = (float)c->unpack_row_len; return 1;
    case GL_UNPACK_SKIP_ROWS: v[0] = (float)c->unpack_skip_rows; return 1;
    case GL_UNPACK_SKIP_PIXELS: v[0] = (float)c->unpack_skip_px; return 1;
    case GL_PACK_ALIGNMENT: v[0] = (float)c->pack_align; return 1;
    case GL_PACK_ROW_LENGTH: v[0] = (float)c->pack_row_len; return 1;
    case GL_PACK_SKIP_ROWS: v[0] = (float)c->pack_skip_rows; return 1;
    case GL_PACK_SKIP_PIXELS: v[0] = (float)c->pack_skip_px; return 1;
    case GL_LIST_BASE: v[0] = (float)c->list_base; return 1;
    case GL_LIST_INDEX: v[0] = (float)c->compiling; return 1;
    case GL_LIST_MODE: v[0] = c->compiling ? (float)c->list_mode : 0; return 1;
    }
    return 0;
}
void glGetFloatv(GLenum p, GLfloat *out) { CTX(); float v[16]; int n = get_state(c, p, v); if (!n) { set_error(GL_INVALID_ENUM); return; } for (int i = 0; i < n; i++) out[i] = v[i]; }
void glGetDoublev(GLenum p, GLdouble *out) { CTX(); float v[16]; int n = get_state(c, p, v); if (!n) { set_error(GL_INVALID_ENUM); return; } for (int i = 0; i < n; i++) out[i] = v[i]; }
void glGetIntegerv(GLenum p, GLint *out) { CTX(); float v[16]; int n = get_state(c, p, v); if (!n) { set_error(GL_INVALID_ENUM); return; } for (int i = 0; i < n; i++) out[i] = (GLint)(v[i] < 0 ? v[i] - 0.5f : v[i] + 0.5f); }
void glGetBooleanv(GLenum p, GLboolean *out) { CTX(); float v[16]; int n = get_state(c, p, v); if (!n) { set_error(GL_INVALID_ENUM); return; } for (int i = 0; i < n; i++) out[i] = v[i] != 0; }

/* ---- matrices ---- */
void glMatrixMode(GLenum m) { CTX(); RECORD(OP_MATRIX_MODE, (float)m); if (m != GL_MODELVIEW && m != GL_PROJECTION && m != GL_TEXTURE) { set_error(GL_INVALID_ENUM); return; } c->matrix_mode = m; }
void glLoadIdentity(void) { CTX(); RECORD(OP_LOAD_IDENTITY, 0); mat_identity(top_matrix(c)); }
void glLoadMatrixf(const GLfloat *m) { CTX(); RECORD_ARRAY(OP_LOAD_MATRIX, m, 16); GL_MEMCPY(top_matrix(c), m, sizeof(mat4)); }
void glMultMatrixf(const GLfloat *m) { CTX(); RECORD_ARRAY(OP_MULT_MATRIX, m, 16); float *t = top_matrix(c); mat_mul(t, t, m); }
void glLoadMatrixd(const GLdouble *m) { mat4 f; for (int i = 0; i < 16; i++) f[i] = (float)m[i]; glLoadMatrixf(f); }
void glMultMatrixd(const GLdouble *m) { mat4 f; for (int i = 0; i < 16; i++) f[i] = (float)m[i]; glMultMatrixf(f); }
static int *stack_top(GLcontext *c, int *max) {
    if (c->matrix_mode == GL_PROJECTION) { *max = MAX_STACK; return &c->pr_top; }
    if (c->matrix_mode == GL_TEXTURE) { *max = MAX_TEX_STACK; return &c->tx_top; }
    *max = MAX_STACK; return &c->mv_top;
}
void glPushMatrix(void) {
    CTX(); RECORD(OP_PUSH_MATRIX, 0);
    int max, *top = stack_top(c, &max);
    if (*top + 1 >= max) { set_error(GL_STACK_OVERFLOW); return; }
    float *src = top_matrix(c); (*top)++; GL_MEMCPY(top_matrix(c), src, sizeof(mat4));
}
void glPopMatrix(void) {
    CTX(); RECORD(OP_POP_MATRIX, 0);
    int max, *top = stack_top(c, &max);
    if (*top == 0) { set_error(GL_STACK_UNDERFLOW); return; }
    (*top)--;
}
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { mat4 m; mat_identity(m); m[12] = x; m[13] = y; m[14] = z; glMultMatrixf(m); }
void glScalef(GLfloat x, GLfloat y, GLfloat z) { mat4 m; mat_identity(m); m[0] = x; m[5] = y; m[10] = z; glMultMatrixf(m); }
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    float len = glm_sqrtf(x * x + y * y + z * z);
    if (len < 1e-6f) return;
    x /= len; y /= len; z /= len;
    float a = angle * GLM_PI / 180.0f, s = glm_sinf(a), co = glm_cosf(a), t = 1 - co;
    mat4 m; mat_identity(m);
    m[0] = t * x * x + co; m[1] = t * x * y + s * z; m[2] = t * x * z - s * y;
    m[4] = t * x * y - s * z; m[5] = t * y * y + co; m[6] = t * y * z + s * x;
    m[8] = t * x * z + s * y; m[9] = t * y * z - s * x; m[10] = t * z * z + co;
    glMultMatrixf(m);
}
void glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((float)x, (float)y, (float)z); }
void glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef((float)x, (float)y, (float)z); }
void glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { glRotatef((float)a, (float)x, (float)y, (float)z); }
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    if (l == r || b == t || n == f) { set_error(GL_INVALID_VALUE); return; }
    mat4 m; mat_identity(m);
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n));
    glMultMatrixf(m);
}
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) { set_error(GL_INVALID_VALUE); return; }
    mat4 m; GL_MEMSET(m, 0, sizeof m);
    m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b)); m[10] = (float)(-(f + n) / (f - n)); m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    glMultMatrixf(m);
}

/* ---- lighting ---- */
void glLightfv(GLenum light, GLenum pname, const GLfloat *p) {
    CTX();
    if (c->compiling) { float a[6] = { (float)light, (float)pname }; for (int i = 0; i < param_count(pname); i++) a[2 + i] = p[i]; RECORD_ARRAY(OP_LIGHT, a, 6); }
    int i = (int)light - GL_LIGHT0; if (i < 0 || i >= MAX_LIGHTS) { set_error(GL_INVALID_ENUM); return; }
    struct light *l = &c->lights[i];
    const float *mv = c->modelview[c->mv_top];
    switch (pname) {
    case GL_AMBIENT: cp4(l->ambient, p); break;
    case GL_DIFFUSE: cp4(l->diffuse, p); break;
    case GL_SPECULAR: cp4(l->specular, p); break;
    case GL_POSITION:                       /* stored in eye space */
        mat_vec(mv, p, l->pos);
        l->dir[0] = l->pos[0]; l->dir[1] = l->pos[1]; l->dir[2] = l->pos[2];
        if (l->pos[3] != 0) { l->dir[0] /= l->pos[3]; l->dir[1] /= l->pos[3]; l->dir[2] /= l->pos[3]; } else normalize3(l->dir);
        break;
    case GL_SPOT_DIRECTION:
        for (int k = 0; k < 3; k++) l->spot_dir[k] = mv[k] * p[0] + mv[4 + k] * p[1] + mv[8 + k] * p[2];
        normalize3(l->spot_dir);
        break;
    case GL_SPOT_EXPONENT: if (p[0] < 0 || p[0] > 128) { set_error(GL_INVALID_VALUE); return; } l->spot_exp = p[0]; break;
    case GL_SPOT_CUTOFF:
        if ((p[0] < 0 || p[0] > 90) && p[0] != 180) { set_error(GL_INVALID_VALUE); return; }
        l->spot_cutoff = p[0]; l->spot_cos = p[0] == 180 ? -1 : glm_cosf(p[0] * GLM_PI / 180.0f);
        break;
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION:
        if (p[0] < 0) { set_error(GL_INVALID_VALUE); return; }
        l->att[pname - GL_CONSTANT_ATTENUATION] = p[0];
        break;
    default: set_error(GL_INVALID_ENUM);
    }
}
void glLightf(GLenum light, GLenum pname, GLfloat v) { float p[4] = { v }; glLightfv(light, pname, p); }
void glLighti(GLenum light, GLenum pname, GLint v) { glLightf(light, pname, (float)v); }
void glGetLightfv(GLenum light, GLenum pname, GLfloat *p) {
    CTX();
    int i = (int)light - GL_LIGHT0; if (i < 0 || i >= MAX_LIGHTS) { set_error(GL_INVALID_ENUM); return; }
    struct light *l = &c->lights[i];
    switch (pname) {
    case GL_AMBIENT: cp4(p, l->ambient); break; case GL_DIFFUSE: cp4(p, l->diffuse); break; case GL_SPECULAR: cp4(p, l->specular); break;
    case GL_POSITION: cp4(p, l->pos); break;
    case GL_SPOT_DIRECTION: p[0] = l->spot_dir[0]; p[1] = l->spot_dir[1]; p[2] = l->spot_dir[2]; break;
    case GL_SPOT_EXPONENT: p[0] = l->spot_exp; break; case GL_SPOT_CUTOFF: p[0] = l->spot_cutoff; break;
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION: p[0] = l->att[pname - GL_CONSTANT_ATTENUATION]; break;
    default: set_error(GL_INVALID_ENUM);
    }
}
void glLightModelfv(GLenum pname, const GLfloat *p) {
    CTX();
    if (c->compiling) { float a[5] = { (float)pname }; for (int i = 0; i < param_count(pname); i++) a[1 + i] = p[i]; RECORD_ARRAY(OP_LIGHT_MODEL, a, 5); }
    if (pname == GL_LIGHT_MODEL_AMBIENT) cp4(c->ambient_model, p);
    else if (pname == GL_LIGHT_MODEL_LOCAL_VIEWER) c->local_viewer = p[0] != 0;
    else if (pname == GL_LIGHT_MODEL_TWO_SIDE) c->two_side = p[0] != 0;
    else set_error(GL_INVALID_ENUM);
}
void glLightModelf(GLenum pname, GLfloat v) { float p[4] = { v }; glLightModelfv(pname, p); }
void glLightModeli(GLenum pname, GLint v) { glLightModelf(pname, (float)v); }
static int set_material(struct material *m, GLenum pname, const float *p) {
    switch (pname) {
    case GL_AMBIENT: cp4(m->ambient, p); return 1;
    case GL_DIFFUSE: cp4(m->diffuse, p); return 1;
    case GL_SPECULAR: cp4(m->specular, p); return 1;
    case GL_EMISSION: cp4(m->emission, p); return 1;
    case GL_AMBIENT_AND_DIFFUSE: cp4(m->ambient, p); cp4(m->diffuse, p); return 1;
    case GL_SHININESS: m->shininess = p[0] < 0 ? 0 : p[0] > 128 ? 128 : p[0]; return 1;
    }
    return 0;
}
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *p) {
    CTX();
    if (c->compiling) { float a[6] = { (float)face, (float)pname }; for (int i = 0; i < param_count(pname); i++) a[2 + i] = p[i]; RECORD_ARRAY(OP_MATERIAL, a, 6); }
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) { set_error(GL_INVALID_ENUM); return; }
    if (face != GL_BACK && !set_material(&c->mat[0], pname, p)) { set_error(GL_INVALID_ENUM); return; }
    if (face != GL_FRONT && !set_material(&c->mat[1], pname, p)) set_error(GL_INVALID_ENUM);
}
void glMaterialf(GLenum face, GLenum pname, GLfloat v) { float p[4] = { v }; glMaterialfv(face, pname, p); }
void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *p) {
    CTX();
    if (face != GL_FRONT && face != GL_BACK) { set_error(GL_INVALID_ENUM); return; }
    struct material *m = &c->mat[face == GL_BACK];
    switch (pname) {
    case GL_AMBIENT: cp4(p, m->ambient); break; case GL_DIFFUSE: cp4(p, m->diffuse); break; case GL_SPECULAR: cp4(p, m->specular); break;
    case GL_EMISSION: cp4(p, m->emission); break; case GL_SHININESS: p[0] = m->shininess; break;
    default: set_error(GL_INVALID_ENUM);
    }
}
void glColorMaterial(GLenum face, GLenum mode) {
    CTX(); RECORD(OP_COLOR_MATERIAL, (float)face, (float)mode);
    if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) ||
        (mode != GL_AMBIENT && mode != GL_DIFFUSE && mode != GL_SPECULAR && mode != GL_EMISSION && mode != GL_AMBIENT_AND_DIFFUSE)) { set_error(GL_INVALID_ENUM); return; }
    c->cm_face = face; c->cm_mode = mode;
    if (c->color_material) apply_color_material(c, c->cur.color);
}
static void apply_color_material(GLcontext *c, const float *col) {
    if (c->cm_face != GL_BACK) set_material(&c->mat[0], c->cm_mode, col);
    if (c->cm_face != GL_FRONT) set_material(&c->mat[1], c->cm_mode, col);
}
void glFogfv(GLenum pname, const GLfloat *p) {
    CTX();
    if (c->compiling) { float a[5] = { (float)pname }; for (int i = 0; i < param_count(pname); i++) a[1 + i] = p[i]; RECORD_ARRAY(OP_FOG, a, 5); }
    switch (pname) {
    case GL_FOG_MODE: { GLenum m = (GLenum)p[0]; if (m != GL_LINEAR && m != GL_EXP && m != GL_EXP2) { set_error(GL_INVALID_ENUM); return; } c->fog_mode = m; break; }
    case GL_FOG_DENSITY: if (p[0] < 0) { set_error(GL_INVALID_VALUE); return; } c->fog_density = p[0]; break;
    case GL_FOG_START: c->fog_start = p[0]; break;
    case GL_FOG_END: c->fog_end = p[0]; break;
    case GL_FOG_COLOR: set4(c->fog_color, clamp01(p[0]), clamp01(p[1]), clamp01(p[2]), clamp01(p[3])); break;
    case GL_FOG_INDEX: break;
    default: set_error(GL_INVALID_ENUM);
    }
}
void glFogf(GLenum pname, GLfloat v) { float p[4] = { v }; glFogfv(pname, p); }
void glFogi(GLenum pname, GLint v) { glFogf(pname, (float)v); }

/* ---- textures ---- */
static struct texture *active_texture(GLcontext *c) {
    if (!c->tex2d) return 0;
    struct texture *t = &c->textures[c->bound_tex];
    return t->lv[0].px ? t : 0;
}
void glGenTextures(GLsizei n, GLuint *out) {
    CTX();
    if (n < 0) { set_error(GL_INVALID_VALUE); return; }
    for (GLsizei k = 0; k < n; k++) {
        out[k] = 0;
        for (GLuint i = 1; i < MAX_TEXTURES; i++) if (!c->textures[i].used) { tex_defaults(&c->textures[i]); out[k] = i; break; }
        if (!out[k]) { set_error(GL_OUT_OF_MEMORY); return; }
    }
}
void glDeleteTextures(GLsizei n, const GLuint *t) {
    CTX();
    for (GLsizei k = 0; k < n; k++) if (t[k] && t[k] < MAX_TEXTURES) {
        tex_free_levels(&c->textures[t[k]]); GL_MEMSET(&c->textures[t[k]], 0, sizeof(struct texture));
        if (c->bound_tex == t[k]) c->bound_tex = 0;
    }
}
GLboolean glIsTexture(GLuint t) { CTX(0); return t && t < MAX_TEXTURES && c->textures[t].used; }
void glBindTexture(GLenum target, GLuint t) {
    CTX(); RECORD(OP_BIND_TEXTURE, (float)target, (float)t);
    if (target != GL_TEXTURE_2D) { set_error(GL_INVALID_ENUM); return; }
    if (t >= MAX_TEXTURES) { set_error(GL_INVALID_VALUE); return; }
    if (!c->textures[t].used) tex_defaults(&c->textures[t]);   /* binding an unused name creates it */
    c->bound_tex = t;
}
static GLenum base_format(GLint internal) {
    switch (internal) {
    case 1: case GL_LUMINANCE: case GL_LUMINANCE8: return GL_LUMINANCE;
    case 2: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE8_ALPHA8: return GL_LUMINANCE_ALPHA;
    case GL_INTENSITY: case GL_INTENSITY8: return GL_INTENSITY;
    case GL_ALPHA: case GL_ALPHA8: return GL_ALPHA;
    case 3: case GL_RGB: case GL_RGB8: case GL_RGB5: return GL_RGB;
    case 4: case GL_RGBA: case GL_RGBA8: case GL_RGBA4: case GL_RGB5_A1: return GL_RGBA;
    }
    return 0;
}
static int format_components(GLenum f) {
    switch (f) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: return 1;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_RGB: case GL_BGR: return 3;
    case GL_RGBA: case GL_BGRA: return 4;
    }
    return 0;
}
static int type_size(GLenum t) {
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
    case GL_DOUBLE: return 8;
    }
    return 0;
}
/* stride between rows of a client image, honouring ROW_LENGTH and ALIGNMENT */
static size_t row_stride(int w, int row_len, int align, int bytes_per_px, int comp_size) {
    size_t s = (size_t)(row_len > 0 ? row_len : w) * bytes_per_px;
    if (comp_size < align) s = (s + align - 1) / align * align;
    return s;
}
/* client pixels -> ARGB texels laid out for the texture's base format */
static uint32_t *unpack_image(GLcontext *c, int w, int h, GLenum format, GLenum type, const void *pixels, GLenum base) {
    uint32_t *out = (size_t)w * h <= MAX_IMAGE_PIXELS ? GL_MALLOC((size_t)w * h * 4) : 0;   /* w, h > 0 are ints: the product cannot wrap */
    if (!out) { set_error(GL_OUT_OF_MEMORY); return 0; }
    if (!pixels) { GL_MEMSET(out, 0, (size_t)w * h * 4); return out; }
    int nc = format_components(format), cs = type_size(type);
    size_t stride = row_stride(w, c->unpack_row_len, c->unpack_align, nc * cs, cs);
    const uint8_t *src = (const uint8_t *)pixels + (size_t)c->unpack_skip_rows * stride + (size_t)c->unpack_skip_px * nc * cs;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            int v[4];
            for (int k = 0; k < nc; k++) {
                const uint8_t *p = row + ((size_t)x * nc + k) * cs;
                v[k] = type == GL_FLOAT ? (int)(clamp01(*(const float *)p) * 255.0f + 0.5f) : *p;
            }
            int r = 0, g = 0, b = 0, a = 255;
            switch (format) {
            case GL_RGB: r = v[0]; g = v[1]; b = v[2]; break;
            case GL_RGBA: r = v[0]; g = v[1]; b = v[2]; a = v[3]; break;
            case GL_BGR: b = v[0]; g = v[1]; r = v[2]; break;
            case GL_BGRA: b = v[0]; g = v[1]; r = v[2]; a = v[3]; break;
            case GL_LUMINANCE: r = g = b = v[0]; break;
            case GL_LUMINANCE_ALPHA: r = g = b = v[0]; a = v[1]; break;
            case GL_ALPHA: a = v[0]; break;
            case GL_RED: r = v[0]; break;
            case GL_GREEN: g = v[0]; break;
            case GL_BLUE: b = v[0]; break;
            }
            switch (base) {   /* luminance-based formats take R as L */
            case GL_LUMINANCE: g = b = r; a = 255; break;
            case GL_LUMINANCE_ALPHA: g = b = r; break;
            case GL_INTENSITY: g = b = a = r; break;
            case GL_ALPHA: r = g = b = 255; break;
            case GL_RGB: a = 255; break;
            }
            out[(size_t)y * w + x] = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
    return out;
}
static void check_mipmaps(struct texture *t) {
    int w = t->lv[0].w, h = t->lv[0].h, n = 1;
    while ((w >> n) || (h >> n)) n++;
    t->mip_complete = n <= MAX_LEVELS;
    for (int i = 1; i < n && t->mip_complete; i++) {
        int lw = w >> i, lh = h >> i;
        if (!t->lv[i].px || t->lv[i].w != (lw ? lw : 1) || t->lv[i].h != (lh ? lh : 1)) t->mip_complete = 0;
    }
    t->nlevels = t->mip_complete ? n : 1;
}
/* store a level of the bound texture; takes ownership of px */
static void tex_store(GLcontext *c, int level, GLenum internal, int w, int h, uint32_t *px) {
    struct texture *t = &c->textures[c->bound_tex];
    if (t->lv[level].px) GL_FREE(t->lv[level].px);
    t->lv[level].px = px; t->lv[level].w = w; t->lv[level].h = h;
    if (level == 0) { t->internal = internal; t->base = base_format(internal); }
    check_mipmaps(t);
}
static uint32_t *dup_pixels(GLcontext *c, const uint32_t *px, size_t n) {
    uint32_t *d = GL_MALLOC(n * 4);
    if (!d) { set_error(GL_OUT_OF_MEMORY); return 0; }
    GL_MEMCPY(d, px, n * 4); return d;
}
/* the recorded half of glTexImage2D: canonical ARGB pixels, ownership passes in */
static void submit_tex_image(GLcontext *c, int level, GLenum internal, int w, int h, uint32_t *px) {
    if (c->compiling) {
        int keep_executing = c->list_mode == GL_COMPILE_AND_EXECUTE;
        uint32_t *copy = keep_executing ? dup_pixels(c, px, (size_t)w * h) : 0;
        int blob = record_blob(c, px);
        if (blob >= 0) { float a[5] = { (float)level, (float)internal, (float)w, (float)h, (float)blob }; record(c, OP_TEX_IMAGE, 5, a); }
        if (!keep_executing || !copy) return;
        px = copy;
    }
    tex_store(c, level, internal, w, h, px);
}
void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const GLvoid *pixels) {
    CTX();
    if (target != GL_TEXTURE_2D) { set_error(GL_INVALID_ENUM); return; }
    GLenum base = base_format(internal);
    if (!base || !format_components(format) || format == GL_DEPTH_COMPONENT || (type != GL_UNSIGNED_BYTE && type != GL_FLOAT)) { set_error(GL_INVALID_ENUM); return; }
    if (level < 0 || level >= MAX_LEVELS || w <= 0 || h <= 0 || w > (MAX_TEX_SIZE >> level) || h > (MAX_TEX_SIZE >> level) || border != 0) { set_error(GL_INVALID_VALUE); return; }
    uint32_t *px = unpack_image(c, w, h, format, type, pixels, base);
    if (px) submit_tex_image(c, level, (GLenum)internal, w, h, px);
}
static void tex_sub_store(GLcontext *c, int level, int x, int y, int w, int h, const uint32_t *px) {
    struct level *l = &c->textures[c->bound_tex].lv[level];
    for (int r = 0; r < h; r++) GL_MEMCPY(l->px + (size_t)(y + r) * l->w + x, px + (size_t)r * w, (size_t)w * 4);
}
void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels) {
    CTX();
    if (target != GL_TEXTURE_2D) { set_error(GL_INVALID_ENUM); return; }
    if (!format_components(format) || format == GL_DEPTH_COMPONENT || (type != GL_UNSIGNED_BYTE && type != GL_FLOAT)) { set_error(GL_INVALID_ENUM); return; }
    /* no level is larger than MAX_TEX_SIZE: bounds the upload (and keeps recorded coordinates exact as floats) */
    if (level < 0 || level >= MAX_LEVELS || x < 0 || y < 0 || w < 0 || h < 0 || w > MAX_TEX_SIZE - x || h > MAX_TEX_SIZE - y) { set_error(GL_INVALID_VALUE); return; }
    if (!w || !h) return;
    struct texture *t = &c->textures[c->bound_tex];
    if (!c->compiling) {
        struct level *l = &t->lv[level];
        if (!l->px) { set_error(GL_INVALID_OPERATION); return; }
        if (w > l->w - x || h > l->h - y) { set_error(GL_INVALID_VALUE); return; }
    }
    uint32_t *px = unpack_image(c, w, h, format, type, pixels, t->base);
    if (!px) return;
    if (c->compiling) {
        int exec = c->list_mode == GL_COMPILE_AND_EXECUTE;
        uint32_t *copy = exec ? dup_pixels(c, px, (size_t)w * h) : 0;
        int blob = record_blob(c, px);
        if (blob >= 0) { float a[6] = { (float)level, (float)x, (float)y, (float)w, (float)h, (float)blob }; record(c, OP_TEX_SUB_IMAGE, 6, a); }
        if (!copy) return;
        px = copy;
        struct level *l = &t->lv[level];
        if (!l->px || w > l->w - x || h > l->h - y) { GL_FREE(px); set_error(GL_INVALID_VALUE); return; }
    }
    tex_sub_store(c, level, x, y, w, h, px);
    GL_FREE(px);
}
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *p) {
    CTX();
    if (c->compiling) { float a[6] = { (float)target, (float)pname }; for (int i = 0; i < param_count(pname); i++) a[2 + i] = p[i]; RECORD_ARRAY(OP_TEX_PARAMETER, a, 6); }
    if (target != GL_TEXTURE_2D) { set_error(GL_INVALID_ENUM); return; }
    struct texture *t = &c->textures[c->bound_tex];
    GLenum v = (GLenum)p[0];
    switch (pname) {
    case GL_TEXTURE_MAG_FILTER: if (v != GL_NEAREST && v != GL_LINEAR) { set_error(GL_INVALID_ENUM); return; } t->mag = v; break;
    case GL_TEXTURE_MIN_FILTER: if (v != GL_NEAREST && v != GL_LINEAR && (v < GL_NEAREST_MIPMAP_NEAREST || v > GL_LINEAR_MIPMAP_LINEAR)) { set_error(GL_INVALID_ENUM); return; } t->min = v; break;
    case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T:
        if (v != GL_REPEAT && v != GL_CLAMP && v != GL_CLAMP_TO_EDGE && v != GL_MIRRORED_REPEAT) { set_error(GL_INVALID_ENUM); return; }
        if (pname == GL_TEXTURE_WRAP_S) t->wrap_s = v; else t->wrap_t = v;
        break;
    case GL_TEXTURE_BORDER_COLOR: break;   /* no border texels: GL_CLAMP behaves as CLAMP_TO_EDGE */
    default: set_error(GL_INVALID_ENUM);
    }
}
void glTexParameterf(GLenum target, GLenum pname, GLfloat v) { float p[4] = { v }; glTexParameterfv(target, pname, p); }
void glTexParameteri(GLenum target, GLenum pname, GLint v) { glTexParameterf(target, pname, (float)v); }
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *p) {
    CTX();
    if (target != GL_TEXTURE_2D) { set_error(GL_INVALID_ENUM); return; }
    if (level < 0 || level >= MAX_LEVELS) { set_error(GL_INVALID_VALUE); return; }
    struct texture *t = &c->textures[c->bound_tex];
    switch (pname) {
    case GL_TEXTURE_WIDTH: p[0] = t->lv[level].px ? t->lv[level].w : 0; break;
    case GL_TEXTURE_HEIGHT: p[0] = t->lv[level].px ? t->lv[level].h : 0; break;
    case GL_TEXTURE_INTERNAL_FORMAT: p[0] = (GLint)t->internal; break;
    case GL_TEXTURE_BORDER: p[0] = 0; break;
    default: set_error(GL_INVALID_ENUM);
    }
}
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *p) {
    CTX();
    if (c->compiling) { float a[6] = { (float)target, (float)pname }; for (int i = 0; i < param_count(pname); i++) a[2 + i] = p[i]; RECORD_ARRAY(OP_TEX_ENV, a, 6); }
    if (target != GL_TEXTURE_ENV) { set_error(GL_INVALID_ENUM); return; }
    if (pname == GL_TEXTURE_ENV_MODE) {
        GLenum m = (GLenum)p[0];
        if (m != GL_MODULATE && m != GL_DECAL && m != GL_BLEND && m != GL_REPLACE && m != GL_ADD) { set_error(GL_INVALID_ENUM); return; }
        c->tex_env = m;
    } else if (pname == GL_TEXTURE_ENV_COLOR) set4(c->tex_env_color, clamp01(p[0]), clamp01(p[1]), clamp01(p[2]), clamp01(p[3]));
    else set_error(GL_INVALID_ENUM);
}
void glTexEnvf(GLenum target, GLenum pname, GLfloat v) { float p[4] = { v }; glTexEnvfv(target, pname, p); }
void glTexEnvi(GLenum target, GLenum pname, GLint v) { glTexEnvf(target, pname, (float)v); }
void glPixelStorei(GLenum pname, GLint v) {
    CTX();
    switch (pname) {
    case GL_UNPACK_ALIGNMENT: case GL_PACK_ALIGNMENT:
        if (v != 1 && v != 2 && v != 4 && v != 8) { set_error(GL_INVALID_VALUE); return; }
        if (pname == GL_UNPACK_ALIGNMENT) c->unpack_align = v; else c->pack_align = v;
        return;
    }
    if (v < 0) { set_error(GL_INVALID_VALUE); return; }
    switch (pname) {
    case GL_UNPACK_ROW_LENGTH: c->unpack_row_len = v; break;
    case GL_UNPACK_SKIP_ROWS: c->unpack_skip_rows = v; break;
    case GL_UNPACK_SKIP_PIXELS: c->unpack_skip_px = v; break;
    case GL_PACK_ROW_LENGTH: c->pack_row_len = v; break;
    case GL_PACK_SKIP_ROWS: c->pack_skip_rows = v; break;
    case GL_PACK_SKIP_PIXELS: c->pack_skip_px = v; break;
    default: set_error(GL_INVALID_ENUM);
    }
}

static inline int wrap_coord(GLenum mode, int i, int n) {
    if (mode == GL_REPEAT) { i %= n; return i < 0 ? i + n : i; }
    if (mode == GL_MIRRORED_REPEAT) { int p = 2 * n; i %= p; if (i < 0) i += p; return i < n ? i : p - 1 - i; }
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}
static inline float prep_coord(GLenum mode, float s) {
    if (mode == GL_REPEAT) return s - fastfloor(s);
    if (mode == GL_MIRRORED_REPEAT) return s - 2 * fastfloor(s * 0.5f);
    return clamp01(s);
}
static void unpack_texel(uint32_t p, float *out) {
    out[0] = ((p >> 16) & 0xFF) * (1.0f / 255); out[1] = ((p >> 8) & 0xFF) * (1.0f / 255); out[2] = (p & 0xFF) * (1.0f / 255); out[3] = (p >> 24) * (1.0f / 255);
}
static void sample_level(const struct texture *t, const struct level *l, int linear, float s, float tt, float *out) {
    int w = l->w, h = l->h;
    s = prep_coord(t->wrap_s, s); tt = prep_coord(t->wrap_t, tt);
    if (!linear) {
        int x = wrap_coord(t->wrap_s, (int)fastfloor(s * w), w), y = wrap_coord(t->wrap_t, (int)fastfloor(tt * h), h);
        unpack_texel(l->px[y * w + x], out);
        return;
    }
    float fx = s * w - 0.5f, fy = tt * h - 0.5f, flx = fastfloor(fx), fly = fastfloor(fy);
    float ax = fx - flx, ay = fy - fly;
    int x0 = wrap_coord(t->wrap_s, (int)flx, w), x1 = wrap_coord(t->wrap_s, (int)flx + 1, w);
    int y0 = wrap_coord(t->wrap_t, (int)fly, h), y1 = wrap_coord(t->wrap_t, (int)fly + 1, h);
    uint32_t p00 = l->px[y0 * w + x0], p10 = l->px[y0 * w + x1], p01 = l->px[y1 * w + x0], p11 = l->px[y1 * w + x1];
    for (int ch = 0; ch < 4; ch++) {
        int sh = ch == 3 ? 24 : 16 - ch * 8;
        float v00 = (p00 >> sh) & 0xFF, v10 = (p10 >> sh) & 0xFF, v01 = (p01 >> sh) & 0xFF, v11 = (p11 >> sh) & 0xFF;
        float top = v00 + (v10 - v00) * ax, bot = v01 + (v11 - v01) * ax;
        out[ch] = (top + (bot - top) * ay) * (1.0f / 255.0f);
    }
}
/* lambda: log2 of the texel/pixel ratio at level 0 (<= 0 means magnification) */
static void tex_sample(const struct texture *t, float s, float tt, float lambda, float *out) {
    GLenum f = lambda <= 0 ? t->mag : t->min;
    int linear = f == GL_LINEAR || f == GL_LINEAR_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_LINEAR;
    if (f == GL_NEAREST || f == GL_LINEAR || !t->mip_complete) { sample_level(t, &t->lv[0], linear, s, tt, out); return; }
    int maxl = t->nlevels - 1;
    if (f == GL_NEAREST_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_NEAREST) {
        int d = (int)(lambda + 0.5f); if (d > maxl) d = maxl;
        sample_level(t, &t->lv[d], linear, s, tt, out);
        return;
    }
    int d = (int)lambda; float fr = lambda - d;
    if (d >= maxl) { sample_level(t, &t->lv[maxl], linear, s, tt, out); return; }
    float a[4], b[4];
    sample_level(t, &t->lv[d], linear, s, tt, a); sample_level(t, &t->lv[d + 1], linear, s, tt, b);
    for (int k = 0; k < 4; k++) out[k] = a[k] + (b[k] - a[k]) * fr;
}
static void tex_env(GLcontext *c, const struct texture *t, const float *tc, float *col) {
    int rgb = t->base != GL_ALPHA, alpha = t->base == GL_RGBA || t->base == GL_ALPHA || t->base == GL_LUMINANCE_ALPHA || t->base == GL_INTENSITY;
    switch (c->tex_env) {
    case GL_REPLACE: if (rgb) { col[0] = tc[0]; col[1] = tc[1]; col[2] = tc[2]; } if (alpha) col[3] = tc[3]; break;
    case GL_DECAL:
        if (t->base == GL_RGBA) for (int k = 0; k < 3; k++) col[k] = col[k] * (1 - tc[3]) + tc[k] * tc[3];
        else if (rgb) { col[0] = tc[0]; col[1] = tc[1]; col[2] = tc[2]; }
        break;
    case GL_BLEND:
        if (rgb) for (int k = 0; k < 3; k++) col[k] = col[k] * (1 - tc[k]) + c->tex_env_color[k] * tc[k];
        if (alpha) col[3] = t->base == GL_INTENSITY ? col[3] * (1 - tc[3]) + c->tex_env_color[3] * tc[3] : col[3] * tc[3];
        break;
    case GL_ADD: if (rgb) for (int k = 0; k < 3; k++) col[k] += tc[k]; if (alpha) col[3] = t->base == GL_INTENSITY ? col[3] + tc[3] : col[3] * tc[3]; break;
    default: if (rgb) for (int k = 0; k < 3; k++) col[k] *= tc[k]; if (alpha) col[3] *= tc[3]; break;   /* GL_MODULATE */
    }
}

/* ---- fragment pipeline ---- */
static inline int compare(GLenum f, float a, float b) {
    switch (f) {
    case GL_NEVER: return 0; case GL_LESS: return a < b; case GL_EQUAL: return a == b; case GL_LEQUAL: return a <= b;
    case GL_GREATER: return a > b; case GL_NOTEQUAL: return a != b; case GL_GEQUAL: return a >= b; default: return 1;
    }
}
static inline float blend_factor(GLenum f, int ch, const float *s, const float *d) {
    switch (f) {
    case GL_ZERO: return 0; case GL_ONE: return 1;
    case GL_SRC_COLOR: return s[ch]; case GL_ONE_MINUS_SRC_COLOR: return 1 - s[ch];
    case GL_DST_COLOR: return d[ch]; case GL_ONE_MINUS_DST_COLOR: return 1 - d[ch];
    case GL_SRC_ALPHA: return s[3]; case GL_ONE_MINUS_SRC_ALPHA: return 1 - s[3];
    case GL_DST_ALPHA: return d[3]; case GL_ONE_MINUS_DST_ALPHA: return 1 - d[3];
    case GL_SRC_ALPHA_SATURATE: { if (ch == 3) return 1; float a = 1 - d[3]; return s[3] < a ? s[3] : a; }
    }
    return 0;
}
/* scissor/viewport, alpha test, depth test, blending, write masks */
static void write_pixel(GLcontext *c, int x, int y, float z, const float *col) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
    float s[4] = { clamp01(col[0]), clamp01(col[1]), clamp01(col[2]), clamp01(col[3]) };
    if (c->alpha_test && !compare(c->alpha_func, s[3], c->alpha_ref)) return;
    size_t idx = (size_t)y * c->width + x;
    if (c->depth_test) {
        if (!compare(c->depth_func, z, c->depth[idx])) return;
        if (c->depth_write) c->depth[idx] = z;
    }
    uint32_t dst = c->color[idx], out;
    if (c->blend) {
        float d[4]; unpack_texel(dst, d);
        float r[4];
        for (int ch = 0; ch < 4; ch++) r[ch] = s[ch] * blend_factor(c->blend_src, ch, s, d) + d[ch] * blend_factor(c->blend_dst, ch, s, d);
        out = pack_color(r[0], r[1], r[2], r[3]);
    } else out = pack_color(s[0], s[1], s[2], s[3]);
    if (c->color_mask != 0xFFFFFFFFu) out = (out & c->color_mask) | (dst & ~c->color_mask);
    c->color[idx] = out;
}
/* texturing and fog for one fragment */
static inline void shade(GLcontext *c, const struct texture *tex, float *col, float s, float t, float lambda, float fog) {
    if (tex) { float tc[4]; tex_sample(tex, s, t, lambda, tc); tex_env(c, tex, tc, col); }
    if (c->fog) for (int k = 0; k < 3; k++) col[k] = col[k] * fog + c->fog_color[k] * (1 - fog);
}
static inline int depth_clipped(GLcontext *c, float z) {
    float lo = c->depth_near < c->depth_far ? c->depth_near : c->depth_far, hi = c->depth_near < c->depth_far ? c->depth_far : c->depth_near;
    return z < lo - 1e-5f || z > hi + 1e-5f;   /* the near and far planes, per fragment */
}

static void draw_point(GLcontext *c, const struct tvertex *v, float zoff) {
    if (depth_clipped(c, v->z)) return;
    float z = clamp01(v->z + zoff);
    int sz = (int)(c->point_size + 0.5f); if (sz < 1) sz = 1; if (sz > 64) sz = 64;
    if (!(v->x > c->cx0 - sz && v->x < c->cx1 + sz && v->y > c->cy0 - sz && v->y < c->cy1 + sz)) return;
    float col[4]; cp4(col, v->c);
    float iq = v->q != 0 ? 1 / v->q : 0;
    shade(c, active_texture(c), col, v->s * iq, v->t * iq, 0, v->fog);
    int x0 = (int)fastfloor(v->x - (sz - 1) * 0.5f), y0 = (int)fastfloor(v->y - (sz - 1) * 0.5f);
    for (int y = y0; y < y0 + sz; y++) for (int x = x0; x < x0 + sz; x++) write_pixel(c, x, y, z, col);
}
static void lerp_tvertex(struct tvertex *o, const struct tvertex *a, const struct tvertex *b, float t) {
    const float *pa = (const float *)a, *pb = (const float *)b; float *po = (float *)o;
    for (size_t i = 0; i < sizeof *o / sizeof(float); i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
}
/* DDA line; the last pixel is left out so connected segments don't overlap */
static void draw_line(GLcontext *c, const struct tvertex *a, const struct tvertex *b, float zoff) {
    int width = (int)(c->line_width + 0.5f); if (width < 1) width = 1; if (width > 64) width = 64;
    /* Liang-Barsky against the clip rectangle (grown by the line width): only w > 0 is clipped
     * geometrically, so endpoints can be very far off screen */
    float x0 = (float)(c->cx0 - width), x1 = (float)(c->cx1 + width), y0 = (float)(c->cy0 - width), y1 = (float)(c->cy1 + width);
    float ex = b->x - a->x, ey = b->y - a->y, t0 = 0, t1 = 1;
    float p[4] = { -ex, ex, -ey, ey }, q[4] = { a->x - x0, x1 - a->x, a->y - y0, y1 - a->y };
    for (int k = 0; k < 4; k++) {
        if (p[k] == 0) { if (q[k] < 0) return; continue; }
        float r = q[k] / p[k];
        if (p[k] < 0) { if (r > t1) return; if (r > t0) t0 = r; } else { if (r < t0) return; if (r < t1) t1 = r; }
    }
    if (!(t0 <= t1)) return;
    struct tvertex ca, cb;
    if (t0 > 0 || t1 < 1) { lerp_tvertex(&ca, a, b, t0); lerp_tvertex(&cb, a, b, t1); a = &ca; b = &cb; }
    float dx = b->x - a->x, dy = b->y - a->y, adx = fabsf_(dx), ady = fabsf_(dy);
    int xmajor = adx >= ady;
    int n = (int)((xmajor ? adx : ady) + 0.5f); if (n < 1) n = 1;
    const struct texture *tex = active_texture(c);
    for (int i = 0; i < n; i++) {
        float t = (float)i / n, z = a->z + (b->z - a->z) * t;
        if (depth_clipped(c, z)) continue;
        z = clamp01(z + zoff);
        float col[4]; for (int k = 0; k < 4; k++) col[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
        float q = a->q + (b->q - a->q) * t, iq = q != 0 ? 1 / q : 0;
        shade(c, tex, col, (a->s + (b->s - a->s) * t) * iq, (a->t + (b->t - a->t) * t) * iq, 0, a->fog + (b->fog - a->fog) * t);
        int px = (int)fastfloor(a->x + dx * t), py = (int)fastfloor(a->y + dy * t);
        for (int k = 0; k < width; k++) { int o = k - (width - 1) / 2; write_pixel(c, xmajor ? px : px + o, xmajor ? py + o : py, z, col); }
    }
}

/* edge function evaluated in a canonical vertex order so triangles sharing an
 * edge compute exactly negated values, keeping the top-left fill rule watertight */
struct edge { float ax, ay, dx, dy, sign; int top_left; };
static void edge_setup(struct edge *e, const struct tvertex *a, const struct tvertex *b) {
    float ex = b->x - a->x, ey = b->y - a->y;
    e->top_left = ey < 0 || (ey == 0 && ex > 0);
    if (a->y > b->y || (a->y == b->y && a->x > b->x)) { const struct tvertex *t = a; a = b; b = t; e->sign = -1; } else e->sign = 1;
    e->ax = a->x; e->ay = a->y; e->dx = b->x - a->x; e->dy = b->y - a->y;
}
static inline float edge_eval(const struct edge *e, float px, float py) { return e->sign * (e->dx * (py - e->ay) - e->dy * (px - e->ax)); }
static inline int edge_inside(const struct edge *e, float v) { return v > 0 || (v == 0 && e->top_left); }

static void fill_triangle(GLcontext *c, const struct tvertex *v0, const struct tvertex *v1, const struct tvertex *v2, float area, float zoff) {
    if (area < 0) { const struct tvertex *t = v1; v1 = v2; v2 = t; area = -area; }
    float minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    if (v1->x < minx) minx = v1->x; if (v1->x > maxx) maxx = v1->x; if (v2->x < minx) minx = v2->x; if (v2->x > maxx) maxx = v2->x;
    if (v1->y < miny) miny = v1->y; if (v1->y > maxy) maxy = v1->y; if (v2->y < miny) miny = v2->y; if (v2->y > maxy) maxy = v2->y;
    if (minx < c->cx0) minx = (float)c->cx0; if (miny < c->cy0) miny = (float)c->cy0;
    if (maxx > c->cx1 - 1) maxx = (float)(c->cx1 - 1); if (maxy > c->cy1 - 1) maxy = (float)(c->cy1 - 1);
    if (!(minx <= maxx) || !(miny <= maxy)) return;   /* also rejects NaN */
    int x0 = (int)fastfloor(minx), x1 = (int)maxx + 1, y0 = (int)fastfloor(miny), y1 = (int)maxy + 1;
    struct edge e0, e1, e2;   /* e_i is the edge opposite v_i */
    edge_setup(&e0, v1, v2); edge_setup(&e1, v2, v0); edge_setup(&e2, v0, v1);
    float ia = 1.0f / area;
    const struct texture *tex = active_texture(c);
    /* perspective-correct texture coordinates: interpolate s/w, t/w, q/w */
    float sw0 = v0->s * v0->iw, sw1 = v1->s * v1->iw, sw2 = v2->s * v2->iw;
    float tw0 = v0->t * v0->iw, tw1 = v1->t * v1->iw, tw2 = v2->t * v2->iw;
    float qw0 = v0->q * v0->iw, qw1 = v1->q * v1->iw, qw2 = v2->q * v2->iw;
    /* screen-space derivatives of the barycentrics, for the mipmap level */
    float b0dx = (v1->y - v2->y) * ia, b0dy = (v2->x - v1->x) * ia, b1dx = (v2->y - v0->y) * ia, b1dy = (v0->x - v2->x) * ia;
    float b2dx = -b0dx - b1dx, b2dy = -b0dy - b1dy;
    float dSdx = b0dx * sw0 + b1dx * sw1 + b2dx * sw2, dSdy = b0dy * sw0 + b1dy * sw1 + b2dy * sw2;
    float dTdx = b0dx * tw0 + b1dx * tw1 + b2dx * tw2, dTdy = b0dy * tw0 + b1dy * tw1 + b2dy * tw2;
    float dQdx = b0dx * qw0 + b1dx * qw1 + b2dx * qw2, dQdy = b0dy * qw0 + b1dy * qw1 + b2dy * qw2;
    int lod = tex && tex->min != tex->mag;
    float tw = tex ? (float)tex->lv[0].w : 0, th = tex ? (float)tex->lv[0].h : 0;
    /* colours and fog are perspective-correct too */
    float cw0[5], cw1[5], cw2[5];
    for (int k = 0; k < 4; k++) { cw0[k] = v0->c[k] * v0->iw; cw1[k] = v1->c[k] * v1->iw; cw2[k] = v2->c[k] * v2->iw; }
    cw0[4] = v0->fog * v0->iw; cw1[4] = v1->fog * v1->iw; cw2[4] = v2->fog * v2->iw;
    for (int y = y0; y < y1; y++) {
        float py = y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f;
            float E0 = edge_eval(&e0, px, py), E1 = edge_eval(&e1, px, py), E2 = edge_eval(&e2, px, py);
            if (!edge_inside(&e0, E0) || !edge_inside(&e1, E1) || !edge_inside(&e2, E2)) continue;
            float b0 = E0 * ia, b1 = E1 * ia, b2 = 1.0f - b0 - b1;
            float z = b0 * v0->z + b1 * v1->z + b2 * v2->z;
            if (depth_clipped(c, z)) continue;
            float pw = b0 * v0->iw + b1 * v1->iw + b2 * v2->iw, ipw = pw != 0 ? 1.0f / pw : 0;
            float col[4];
            for (int k = 0; k < 4; k++) col[k] = (b0 * cw0[k] + b1 * cw1[k] + b2 * cw2[k]) * ipw;
            float fog = (b0 * cw0[4] + b1 * cw1[4] + b2 * cw2[4]) * ipw;
            float s = 0, t = 0, lambda = 0;
            if (tex) {
                float S = b0 * sw0 + b1 * sw1 + b2 * sw2, T = b0 * tw0 + b1 * tw1 + b2 * tw2, Q = b0 * qw0 + b1 * qw1 + b2 * qw2;
                float iq = Q != 0 ? 1.0f / Q : 0;
                s = S * iq; t = T * iq;
                if (lod) {
                    float dsdx = (dSdx - s * dQdx) * iq * tw, dtdx = (dTdx - t * dQdx) * iq * th;
                    float dsdy = (dSdy - s * dQdy) * iq * tw, dtdy = (dTdy - t * dQdy) * iq * th;
                    float rx = dsdx * dsdx + dtdx * dtdx, ry = dsdy * dsdy + dtdy * dtdy;
                    lambda = 0.5f * log2f_(rx > ry ? rx : ry);
                }
            }
            shade(c, tex, col, s, t, lambda, fog);
            write_pixel(c, x, y, clamp01(z + zoff), col);
        }
    }
}

/* edge mask bits: 1 = v0-v1, 2 = v1-v2, 4 = v2-v0 (polygon boundary edges, for line/point mode) */
static void draw_triangle(GLcontext *c, const struct tvertex *v0, const struct tvertex *v1, const struct tvertex *v2, int mask) {
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0) return;
    /* screen y points down, so CCW in GL becomes negative area here */
    int is_front = c->front_face == GL_CCW ? area < 0 : area > 0;
    if (c->cull && (c->cull_face == GL_FRONT_AND_BACK || (c->cull_face == GL_BACK) != is_front)) return;
    struct tvertex bv[3];
    if (!is_front && c->two_side && c->lighting) {   /* back faces take the back-material colours */
        const struct tvertex *src[3] = { v0, v1, v2 };
        for (int i = 0; i < 3; i++) { bv[i] = *src[i]; cp4(bv[i].c, src[i]->bc); }
        v0 = &bv[0]; v1 = &bv[1]; v2 = &bv[2];
    }
    GLenum mode = is_front ? c->poly_front : c->poly_back;
    int offset = mode == GL_FILL ? c->offset_fill : mode == GL_LINE ? c->offset_line : c->offset_point;
    float zoff = 0;
    if (offset) {
        float dzdx = ((v1->z - v0->z) * (v2->y - v0->y) - (v2->z - v0->z) * (v1->y - v0->y)) / area;
        float dzdy = ((v1->x - v0->x) * (v2->z - v0->z) - (v2->x - v0->x) * (v1->z - v0->z)) / area;
        float m = fabsf_(dzdx) > fabsf_(dzdy) ? fabsf_(dzdx) : fabsf_(dzdy);
        zoff = c->offset_factor * m + c->offset_units * (1.0f / 16777216.0f);
    }
    if (mode == GL_LINE) {
        if (mask & 1) draw_line(c, v0, v1, zoff);
        if (mask & 2) draw_line(c, v1, v2, zoff);
        if (mask & 4) draw_line(c, v2, v0, zoff);
    } else if (mode == GL_POINT) {
        if (mask & 1) draw_point(c, v0, zoff);
        if (mask & 2) draw_point(c, v1, zoff);
        if (mask & 4) draw_point(c, v2, zoff);
    } else fill_triangle(c, v0, v1, v2, area, zoff);
}

/* ---- primitive assembly ---- */
static void to_screen(GLcontext *c, const struct cvert *v, struct tvertex *out) {
    float iw = 1.0f / v->clip[3];
    float nx = v->clip[0] * iw, ny = v->clip[1] * iw, nz = v->clip[2] * iw;
    out->x = c->vp_x + (nx + 1) * 0.5f * c->vp_w;
    out->y = c->height - (c->vp_y + (ny + 1) * 0.5f * c->vp_h);
    out->z = c->depth_near + (c->depth_far - c->depth_near) * (nz + 1) * 0.5f;
    out->iw = iw;
    cp4(out->c, v->col); cp4(out->bc, v->bcol);
    out->s = v->tex[0]; out->t = v->tex[1]; out->q = v->tex[3]; out->fog = v->fog;
}
static void lerp_cvert(struct cvert *o, const struct cvert *a, const struct cvert *b, float t) {
    const float *pa = (const float *)a, *pb = (const float *)b; float *po = (float *)o;
    for (size_t i = 0; i < sizeof *o / sizeof(float); i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
}
/* signed distance to the near plane (z >= -w) or to w >= eps, which guards odd projections */
static inline float plane_dist(const struct cvert *v, int plane) { return plane ? v->clip[3] - CLIP_W_EPS : v->clip[2] + v->clip[3]; }
/* Sutherland-Hodgman against one plane; e[i] flags the edge from vertex i to i+1 as a polygon boundary */
static int clip_plane(const struct cvert *in, const int *ie, int n, struct cvert *out, int *oe, int plane) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const struct cvert *p = &in[i], *q = &in[(i + 1) % n];
        float dp = plane_dist(p, plane), dq = plane_dist(q, plane);
        if (dp >= 0) { out[m] = *p; oe[m++] = ie[i]; }
        if ((dp >= 0) != (dq >= 0)) { lerp_cvert(&out[m], p, q, dp / (dp - dq)); oe[m++] = dp >= 0 ? 0 : ie[i]; }
    }
    return m;
}
/* a triangle or quad: clip against the near plane (left/right/top/bottom are handled by the
 * clip rectangle and far by the per-fragment depth check), then fan */
static void emit_poly(GLcontext *c, const struct cvert *const *in, int n, const int *edge, const struct cvert *pv) {
    /* a pass keeps i inside vertices and adds one per crossing edge, at most 1.5n: a non-planar quad can reach 4 -> 6 -> 9 */
    struct cvert a[9], out[9]; int ae[9], oe[9], m = n;
    int need = 0;
    for (int i = 0; i < n; i++) { a[i] = *in[i]; ae[i] = edge[i]; if (plane_dist(&a[i], 0) < 0 || plane_dist(&a[i], 1) < 0) need = 1; }
    if (need) {
        m = clip_plane(a, ae, n, out, oe, 0);
        if (m < 3) return;
        m = clip_plane(out, oe, m, a, ae, 1);
        if (m < 3) return;
    }
    if (pv) for (int i = 0; i < m; i++) { cp4(a[i].col, pv->col); cp4(a[i].bcol, pv->bcol); }   /* flat shading */
    struct tvertex sv[9];
    for (int i = 0; i < m; i++) to_screen(c, &a[i], &sv[i]);
    for (int k = 1; k + 1 < m; k++)
        draw_triangle(c, &sv[0], &sv[k], &sv[k + 1], (k == 1 ? ae[0] : 0) | ae[k] << 1 | (k + 2 == m ? ae[m - 1] : 0) << 2);
}
static void emit_tri(GLcontext *c, const struct cvert *a, const struct cvert *b, const struct cvert *d, const int *edge, const struct cvert *pv) {
    const struct cvert *in[3] = { a, b, d };
    emit_poly(c, in, 3, edge, pv);
}
static void emit_line(GLcontext *c, const struct cvert *a, const struct cvert *b, const struct cvert *pv) {
    struct cvert p = *a, q = *b;
    for (int plane = 0; plane < 2; plane++) {
        float dp = plane_dist(&p, plane), dq = plane_dist(&q, plane);
        if (dp < 0 && dq < 0) return;
        if (dp < 0) lerp_cvert(&p, &q, &p, dq / (dq - dp));
        else if (dq < 0) lerp_cvert(&q, &p, &q, dp / (dp - dq));
    }
    if (pv) { cp4(p.col, pv->col); cp4(q.col, pv->col); }
    struct tvertex s0, s1;
    to_screen(c, &p, &s0); to_screen(c, &q, &s1);
    draw_line(c, &s0, &s1, 0);
}

/* ---- vertex processing ---- */
static void light_vertex(GLcontext *c, int back, const float *ep, const float *nrm, const float *vcol, float *out) {
    struct material m = c->mat[back];
    if (c->color_material && (c->cm_face == GL_FRONT_AND_BACK || c->cm_face == (back ? GL_BACK : GL_FRONT))) set_material(&m, c->cm_mode, vcol);
    float n[3] = { back ? -nrm[0] : nrm[0], back ? -nrm[1] : nrm[1], back ? -nrm[2] : nrm[2] };
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = m.emission[k] + c->ambient_model[k] * m.ambient[k];
    float V[3] = { 0, 0, 1 };
    if (c->local_viewer) { V[0] = -ep[0]; V[1] = -ep[1]; V[2] = -ep[2]; normalize3(V); }
    int has_spec = m.specular[0] != 0 || m.specular[1] != 0 || m.specular[2] != 0;
    for (int i = 0; i < MAX_LIGHTS; i++) {
        const struct light *l = &c->lights[i]; if (!l->enabled) continue;
        float L[3], att = 1;
        if (l->pos[3] == 0) { L[0] = l->dir[0]; L[1] = l->dir[1]; L[2] = l->dir[2]; }
        else {
            L[0] = l->dir[0] - ep[0]; L[1] = l->dir[1] - ep[1]; L[2] = l->dir[2] - ep[2];
            float d2 = dot3(L, L), d = glm_sqrtf(d2);
            if (d > 0) { L[0] /= d; L[1] /= d; L[2] /= d; }
            if (l->att[1] != 0 || l->att[2] != 0 || l->att[0] != 1) { float den = l->att[0] + l->att[1] * d + l->att[2] * d2; att = den > 0 ? 1 / den : 0; }
            if (l->spot_cutoff != 180) {
                float sd = -dot3(L, l->spot_dir);
                if (sd < l->spot_cos) continue;
                att *= glm_powf(sd, l->spot_exp);
            }
        }
        float nl = dot3(n, L), diff = nl > 0 ? nl : 0;
        for (int k = 0; k < 3; k++) r[k] += att * (l->ambient[k] * m.ambient[k] + diff * l->diffuse[k] * m.diffuse[k]);
        if (nl > 0 && has_spec) {
            float H[3] = { L[0] + V[0], L[1] + V[1], L[2] + V[2] }; normalize3(H);
            float nh = dot3(n, H);
            if (nh > 0) { float sp = att * glm_powf(nh, m.shininess); for (int k = 0; k < 3; k++) r[k] += sp * l->specular[k] * m.specular[k]; }
        }
    }
    out[0] = clamp01(r[0]); out[1] = clamp01(r[1]); out[2] = clamp01(r[2]); out[3] = clamp01(m.diffuse[3]);
}
static float fog_factor(GLcontext *c, float dist) {
    float f;
    if (c->fog_mode == GL_LINEAR) f = c->fog_end != c->fog_start ? (c->fog_end - dist) / (c->fog_end - c->fog_start) : 1;
    else if (c->fog_mode == GL_EXP) f = expf_(-c->fog_density * dist);
    else { float d = c->fog_density * dist; f = expf_(-d * d); }
    return clamp01(f);
}
struct xform { float nm[9]; int tex_identity; };   /* normal matrix (inverse transpose of the modelview's 3x3, row-major) */
static void xform_setup(GLcontext *c, struct xform *x) {
    const float *m = c->modelview[c->mv_top];
    float a00 = m[0], a01 = m[4], a02 = m[8], a10 = m[1], a11 = m[5], a12 = m[9], a20 = m[2], a21 = m[6], a22 = m[10];
    float c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
    float c10 = a02 * a21 - a01 * a22, c11 = a00 * a22 - a02 * a20, c12 = a01 * a20 - a00 * a21;
    float c20 = a01 * a12 - a02 * a11, c21 = a02 * a10 - a00 * a12, c22 = a00 * a11 - a01 * a10;
    float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (det == 0) { float id[9] = { a00, a01, a02, a10, a11, a12, a20, a21, a22 }; GL_MEMCPY(x->nm, id, sizeof id); }
    else { float inv = 1 / det, cf[9] = { c00, c01, c02, c10, c11, c12, c20, c21, c22 }; for (int i = 0; i < 9; i++) x->nm[i] = cf[i] * inv; }
    x->tex_identity = mat_is_identity(c->texmat[c->tx_top]);
}
static void process_vertex(GLcontext *c, const struct xform *x, const struct vertex *v, struct cvert *o) {
    float eye[4]; mat_vec(c->modelview[c->mv_top], v->pos, eye);
    mat_vec(c->projection[c->pr_top], eye, o->clip);
    if (c->lighting) {
        const float *nm = x->nm;
        float n[3] = { nm[0] * v->normal[0] + nm[1] * v->normal[1] + nm[2] * v->normal[2],
                       nm[3] * v->normal[0] + nm[4] * v->normal[1] + nm[5] * v->normal[2],
                       nm[6] * v->normal[0] + nm[7] * v->normal[1] + nm[8] * v->normal[2] };
        normalize3(n);   /* always normalised, so GL_NORMALIZE and GL_RESCALE_NORMAL are implied */
        float iw = eye[3] != 0 ? 1 / eye[3] : 1, ep[3] = { eye[0] * iw, eye[1] * iw, eye[2] * iw };
        light_vertex(c, 0, ep, n, v->color, o->col);
        if (c->two_side) light_vertex(c, 1, ep, n, v->color, o->bcol); else cp4(o->bcol, o->col);
    } else { cp4(o->col, v->color); cp4(o->bcol, v->color); }
    if (x->tex_identity) cp4(o->tex, v->tex); else mat_vec(c->texmat[c->tx_top], v->tex, o->tex);
    o->fog = c->fog ? fog_factor(c, fabsf_(eye[2])) : 1;
}
static void flush_primitives(GLcontext *c) {
    int n = c->nverts;
    if (!n) return;
    c->nverts = 0;
    update_clip(c);
    if (c->cx0 >= c->cx1 || c->cy0 >= c->cy1) return;
    struct xform x; xform_setup(c, &x);
    struct cvert *cv = c->cverts;
    for (int i = 0; i < n; i++) process_vertex(c, &x, &c->verts[i], &cv[i]);
    int flat = c->shade == GL_FLAT;
    static const int all[4] = { 1, 1, 1, 1 };
    /* the provoking vertex supplies the colour of a flat-shaded primitive */
    #define PV(i) (flat ? &cv[i] : 0)
    switch (c->prim_mode) {
    case GL_POINTS:
        for (int i = 0; i < n; i++) if (plane_dist(&cv[i], 0) >= 0 && plane_dist(&cv[i], 1) >= 0) { struct tvertex s; to_screen(c, &cv[i], &s); draw_point(c, &s, 0); }
        break;
    case GL_LINES: for (int i = 0; i + 1 < n; i += 2) emit_line(c, &cv[i], &cv[i + 1], PV(i + 1)); break;
    case GL_LINE_STRIP: for (int i = 0; i + 1 < n; i++) emit_line(c, &cv[i], &cv[i + 1], PV(i + 1)); break;
    case GL_LINE_LOOP: if (n > 1) for (int i = 0; i < n; i++) emit_line(c, &cv[i], &cv[(i + 1) % n], PV((i + 1) % n)); break;
    case GL_TRIANGLES: for (int i = 0; i + 2 < n; i += 3) emit_tri(c, &cv[i], &cv[i + 1], &cv[i + 2], all, PV(i + 2)); break;
    case GL_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < n; i++) {
            if (i & 1) emit_tri(c, &cv[i + 1], &cv[i], &cv[i + 2], all, PV(i + 2));
            else emit_tri(c, &cv[i], &cv[i + 1], &cv[i + 2], all, PV(i + 2));
        }
        break;
    case GL_TRIANGLE_FAN: for (int i = 1; i + 1 < n; i++) emit_tri(c, &cv[0], &cv[i], &cv[i + 1], all, PV(i + 1)); break;
    case GL_QUADS:
        for (int i = 0; i + 3 < n; i += 4) { const struct cvert *q[4] = { &cv[i], &cv[i + 1], &cv[i + 2], &cv[i + 3] }; emit_poly(c, q, 4, all, PV(i + 3)); }
        break;
    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < n; i += 2) { const struct cvert *q[4] = { &cv[i], &cv[i + 1], &cv[i + 3], &cv[i + 2] }; emit_poly(c, q, 4, all, PV(i + 3)); }
        break;
    case GL_POLYGON:
        for (int i = 1; i + 1 < n; i++) { int e[3] = { i == 1, 1, i + 2 == n }; emit_tri(c, &cv[0], &cv[i], &cv[i + 1], e, PV(0)); }
        break;
    }
    #undef PV
}

/* ---- immediate mode ---- */
void glBegin(GLenum mode) {
    CTX(); RECORD(OP_BEGIN, (float)mode);
    if (mode > GL_POLYGON) { set_error(GL_INVALID_ENUM); return; }
    if (c->in_begin) { set_error(GL_INVALID_OPERATION); return; }
    c->in_begin = 1; c->prim_mode = mode; c->nverts = 0;
}
void glEnd(void) { CTX(); RECORD(OP_END, 0); if (!c->in_begin) { set_error(GL_INVALID_OPERATION); return; } flush_primitives(c); c->in_begin = 0; }
static int grow_verts(GLcontext *c) {
    int ncap = c->cap_verts * 2;
    if (ncap > MAX_VERTS) return 0;
    struct vertex *v = gl_grow(c->verts, (size_t)c->nverts * sizeof *v, (size_t)ncap * sizeof *v);
    if (!v) return 0;
    c->verts = v;
    struct cvert *cv = GL_MALLOC((size_t)ncap * sizeof *cv);
    if (!cv) return 0;
    GL_FREE(c->cverts); c->cverts = cv;
    c->cap_verts = ncap;
    return 1;
}
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    CTX(); RECORD(OP_VERTEX, x, y, z, w);
    if (!c->in_begin) { set_error(GL_INVALID_OPERATION); return; }
    if (c->nverts == c->cap_verts) {
        GLenum m = c->prim_mode;   /* independent primitives flush on a primitive boundary; connected ones grow the buffer */
        if (m == GL_POINTS || m == GL_LINES || m == GL_TRIANGLES || m == GL_QUADS) flush_primitives(c);
        else if (!grow_verts(c)) { set_error(GL_OUT_OF_MEMORY); return; }
    }
    struct vertex *v = &c->verts[c->nverts++];
    *v = c->cur; v->pos[0] = x; v->pos[1] = y; v->pos[2] = z; v->pos[3] = w;
}
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { glVertex4f(x, y, z, 1); }
void glVertex2f(GLfloat x, GLfloat y) { glVertex4f(x, y, 0, 1); }
void glVertex2i(GLint x, GLint y) { glVertex4f((float)x, (float)y, 0, 1); }
void glVertex2d(GLdouble x, GLdouble y) { glVertex4f((float)x, (float)y, 0, 1); }
void glVertex2fv(const GLfloat *v) { glVertex4f(v[0], v[1], 0, 1); }
void glVertex3i(GLint x, GLint y, GLint z) { glVertex4f((float)x, (float)y, (float)z, 1); }
void glVertex3d(GLdouble x, GLdouble y, GLdouble z) { glVertex4f((float)x, (float)y, (float)z, 1); }
void glVertex3fv(const GLfloat *v) { glVertex4f(v[0], v[1], v[2], 1); }
void glVertex3dv(const GLdouble *v) { glVertex4f((float)v[0], (float)v[1], (float)v[2], 1); }
void glVertex4fv(const GLfloat *v) { glVertex4f(v[0], v[1], v[2], v[3]); }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    CTX(); RECORD(OP_COLOR, r, g, b, a);
    set4(c->cur.color, r, g, b, a);
    if (c->color_material) apply_color_material(c, c->cur.color);
}
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1); }
void glColor3d(GLdouble r, GLdouble g, GLdouble b) { glColor4f((float)r, (float)g, (float)b, 1); }
void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a) { glColor4f((float)r, (float)g, (float)b, (float)a); }
void glColor3fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], 1); }
void glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1); }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
void glColor3ubv(const GLubyte *v) { glColor3ub(v[0], v[1], v[2]); }
void glColor4ubv(const GLubyte *v) { glColor4ub(v[0], v[1], v[2], v[3]); }
void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { CTX(); RECORD(OP_NORMAL, x, y, z); c->cur.normal[0] = x; c->cur.normal[1] = y; c->cur.normal[2] = z; }
void glNormal3d(GLdouble x, GLdouble y, GLdouble z) { glNormal3f((float)x, (float)y, (float)z); }
void glNormal3fv(const GLfloat *n) { glNormal3f(n[0], n[1], n[2]); }
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { CTX(); RECORD(OP_TEXCOORD, s, t, r, q); set4(c->cur.tex, s, t, r, q); }
void glTexCoord1f(GLfloat s) { glTexCoord4f(s, 0, 0, 1); }
void glTexCoord2f(GLfloat s, GLfloat t) { glTexCoord4f(s, t, 0, 1); }
void glTexCoord2d(GLdouble s, GLdouble t) { glTexCoord4f((float)s, (float)t, 0, 1); }
void glTexCoord2fv(const GLfloat *v) { glTexCoord4f(v[0], v[1], 0, 1); }
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { glTexCoord4f(s, t, r, 1); }
void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) { glBegin(GL_POLYGON); glVertex2f(x1, y1); glVertex2f(x2, y1); glVertex2f(x2, y2); glVertex2f(x1, y2); glEnd(); }
void glRecti(GLint x1, GLint y1, GLint x2, GLint y2) { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }

/* ---- vertex arrays ---- */
void glEnableClientState(GLenum a) { CTX(); struct array *p = client_array(c, a); if (p) p->enabled = 1; else set_error(GL_INVALID_ENUM); }
void glDisableClientState(GLenum a) { CTX(); struct array *p = client_array(c, a); if (p) p->enabled = 0; else set_error(GL_INVALID_ENUM); }
static void set_array(GLcontext *c, struct array *a, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr, int min_size, int max_size) {
    if (size < min_size || size > max_size || stride < 0) { set_error(GL_INVALID_VALUE); return; }
    if (!type_size(type)) { set_error(GL_INVALID_ENUM); return; }
    a->size = size; a->type = type; a->stride = stride; a->ptr = ptr;
}
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    CTX();
    if (type == GL_BYTE || type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT) { set_error(GL_INVALID_ENUM); return; }
    set_array(c, &c->va_vertex, size, type, stride, ptr, 2, 4);
}
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { CTX(); set_array(c, &c->va_color, size, type, stride, ptr, 3, 4); }
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr) {
    CTX();
    if (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT) { set_error(GL_INVALID_ENUM); return; }
    set_array(c, &c->va_normal, 3, type, stride, ptr, 3, 3);
}
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    CTX();
    if (type == GL_BYTE || type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT) { set_error(GL_INVALID_ENUM); return; }
    set_array(c, &c->va_tex, size, type, stride, ptr, 1, 4);
}
/* read one element; integer colours and normals are normalised to [0,1] / [-1,1] */
static void fetch(const struct array *a, int idx, int normalized, float *out) {
    int ts = type_size(a->type);
    const uint8_t *p = (const uint8_t *)a->ptr + (size_t)idx * (a->stride ? (size_t)a->stride : (size_t)a->size * ts);
    for (int k = 0; k < a->size; k++, p += ts) {
        float v;
        switch (a->type) {
        case GL_BYTE: v = *(const int8_t *)p; if (normalized) v = (2 * v + 1) / 255.0f; break;
        case GL_UNSIGNED_BYTE: v = *p; if (normalized) v /= 255.0f; break;
        case GL_SHORT: v = *(const int16_t *)p; if (normalized) v = (2 * v + 1) / 65535.0f; break;
        case GL_UNSIGNED_SHORT: v = *(const uint16_t *)p; if (normalized) v /= 65535.0f; break;
        case GL_INT: v = (float)*(const int32_t *)p; if (normalized) v = (float)((2.0 * *(const int32_t *)p + 1) / 4294967295.0); break;
        case GL_UNSIGNED_INT: v = (float)*(const uint32_t *)p; if (normalized) v = (float)(*(const uint32_t *)p / 4294967295.0); break;
        case GL_DOUBLE: v = (float)*(const double *)p; break;
        default: v = *(const float *)p; break;
        }
        out[k] = v;
    }
}
void glArrayElement(GLint i) {
    CTX();
    if (c->va_color.enabled && c->va_color.ptr) { float v[4] = { 0, 0, 0, 1 }; fetch(&c->va_color, i, 1, v); glColor4f(v[0], v[1], v[2], v[3]); }
    if (c->va_normal.enabled && c->va_normal.ptr) { float v[3]; fetch(&c->va_normal, i, 1, v); glNormal3f(v[0], v[1], v[2]); }
    if (c->va_tex.enabled && c->va_tex.ptr) { float v[4] = { 0, 0, 0, 1 }; fetch(&c->va_tex, i, 0, v); glTexCoord4f(v[0], v[1], v[2], v[3]); }
    if (c->va_vertex.enabled && c->va_vertex.ptr) { float v[4] = { 0, 0, 0, 1 }; fetch(&c->va_vertex, i, 0, v); glVertex4f(v[0], v[1], v[2], v[3]); }
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    CTX();
    if (mode > GL_POLYGON) { set_error(GL_INVALID_ENUM); return; }
    if (count < 0 || first < 0) { set_error(GL_INVALID_VALUE); return; }
    if (c->in_begin) { set_error(GL_INVALID_OPERATION); return; }
    glBegin(mode);
    for (GLsizei i = 0; i < count; i++) glArrayElement(first + i);
    glEnd();
}
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    CTX();
    if (mode > GL_POLYGON || (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT)) { set_error(GL_INVALID_ENUM); return; }
    if (count < 0) { set_error(GL_INVALID_VALUE); return; }
    if (c->in_begin) { set_error(GL_INVALID_OPERATION); return; }
    glBegin(mode);
    for (GLsizei i = 0; i < count; i++)
        glArrayElement(type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] : type == GL_UNSIGNED_SHORT ? ((const uint16_t *)indices)[i] : (GLint)((const uint32_t *)indices)[i]);
    glEnd();
}
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices) {
    if (end < start) { set_error(GL_INVALID_VALUE); return; }
    glDrawElements(mode, count, type, indices);
}

/* ---- display lists ---- */
static int ensure_lists(GLcontext *c, GLuint n) {
    if (n <= (GLuint)c->nlists) return 1;
    if (n > MAX_LISTS) return 0;
    GLuint ncap = c->nlists ? (GLuint)c->nlists : 64;
    while (ncap < n) ncap *= 2;
    if (ncap > MAX_LISTS) ncap = MAX_LISTS;
    struct dlist *l = gl_grow(c->lists, (size_t)c->nlists * sizeof *l, (size_t)ncap * sizeof *l);
    if (!l) return 0;
    GL_MEMSET(l + c->nlists, 0, (size_t)(ncap - c->nlists) * sizeof *l);
    c->lists = l; c->nlists = (int)ncap;
    return 1;
}
GLuint glGenLists(GLsizei range) {
    CTX(0);
    if (range < 0) { set_error(GL_INVALID_VALUE); return 0; }
    if (range == 0 || c->in_begin) { if (c->in_begin) set_error(GL_INVALID_OPERATION); return 0; }
    for (GLuint first = 1; first + (GLuint)range <= MAX_LISTS; first++) {
        GLsizei k = 0;
        while (k < range && (first + k >= (GLuint)c->nlists || !c->lists[first + k].used) && !(c->compiling && first + k == c->compiling)) k++;
        if (k < range) { first += k; continue; }
        if (!ensure_lists(c, first + range)) break;
        for (k = 0; k < range; k++) c->lists[first + k].used = 1;
        return first;
    }
    set_error(GL_OUT_OF_MEMORY);
    return 0;
}
void glDeleteLists(GLuint list, GLsizei range) {
    CTX();
    if (range < 0) { set_error(GL_INVALID_VALUE); return; }
    if (list >= (GLuint)c->nlists) return;
    if ((GLuint)range > (GLuint)c->nlists - list) range = (GLsizei)((GLuint)c->nlists - list);   /* no list + k wrap-around */
    for (GLsizei k = 0; k < range; k++) dlist_free(&c->lists[list + k]);
}
GLboolean glIsList(GLuint list) { CTX(0); return list && list < (GLuint)c->nlists && c->lists[list].used; }
void glNewList(GLuint list, GLenum mode) {
    CTX();
    if (!list || list >= MAX_LISTS) { set_error(GL_INVALID_VALUE); return; }   /* also keeps list + 1 from wrapping */
    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) { set_error(GL_INVALID_ENUM); return; }
    if (c->compiling || c->in_begin) { set_error(GL_INVALID_OPERATION); return; }
    if (!ensure_lists(c, list + 1)) { set_error(GL_OUT_OF_MEMORY); return; }
    dlist_free(&c->building);
    c->compiling = list; c->list_mode = mode;
}
void glEndList(void) {
    CTX();
    if (!c->compiling) { set_error(GL_INVALID_OPERATION); return; }
    struct dlist *l = &c->lists[c->compiling];   /* the list is replaced only now, as the spec requires */
    dlist_free(l);
    *l = c->building; l->used = 1;
    GL_MEMSET(&c->building, 0, sizeof c->building);
    c->compiling = 0;
}
static void execute_list(GLcontext *c, GLuint id) {
    if (id >= (GLuint)c->nlists || !c->lists[id].used || c->list_depth >= MAX_LIST_NESTING) return;
    GLuint saved = c->compiling;
    c->compiling = 0; c->list_depth++;
    const struct dlist *l = &c->lists[id];
    for (int i = 0; i < l->len; ) {
        uint32_t h = l->w[i].u; int op = (int)(h & 0xFF), n = (int)(h >> 8);
        float a[16];
        for (int k = 0; k < n && k < 16; k++) a[k] = l->w[i + 1 + k].f;
        i += 1 + n;
        #define E(k) ((GLenum)a[k])
        switch (op) {
        case OP_CLEAR: glClear(E(0)); break;
        case OP_CLEAR_COLOR: glClearColor(a[0], a[1], a[2], a[3]); break;
        case OP_CLEAR_DEPTH: glClearDepth(a[0]); break;
        case OP_VIEWPORT: glViewport((GLint)a[0], (GLint)a[1], (GLsizei)a[2], (GLsizei)a[3]); break;
        case OP_SCISSOR: glScissor((GLint)a[0], (GLint)a[1], (GLsizei)a[2], (GLsizei)a[3]); break;
        case OP_DEPTH_RANGE: glDepthRange(a[0], a[1]); break;
        case OP_ENABLE: glEnable(E(0)); break;
        case OP_DISABLE: glDisable(E(0)); break;
        case OP_DEPTH_FUNC: glDepthFunc(E(0)); break;
        case OP_DEPTH_MASK: glDepthMask((GLboolean)a[0]); break;
        case OP_COLOR_MASK: glColorMask((GLboolean)a[0], (GLboolean)a[1], (GLboolean)a[2], (GLboolean)a[3]); break;
        case OP_ALPHA_FUNC: glAlphaFunc(E(0), a[1]); break;
        case OP_CULL_FACE: glCullFace(E(0)); break;
        case OP_FRONT_FACE: glFrontFace(E(0)); break;
        case OP_SHADE_MODEL: glShadeModel(E(0)); break;
        case OP_BLEND_FUNC: glBlendFunc(E(0), E(1)); break;
        case OP_POLYGON_MODE: glPolygonMode(E(0), E(1)); break;
        case OP_POLYGON_OFFSET: glPolygonOffset(a[0], a[1]); break;
        case OP_POINT_SIZE: glPointSize(a[0]); break;
        case OP_LINE_WIDTH: glLineWidth(a[0]); break;
        case OP_MATRIX_MODE: glMatrixMode(E(0)); break;
        case OP_LOAD_IDENTITY: glLoadIdentity(); break;
        case OP_LOAD_MATRIX: glLoadMatrixf(a); break;
        case OP_MULT_MATRIX: glMultMatrixf(a); break;
        case OP_PUSH_MATRIX: glPushMatrix(); break;
        case OP_POP_MATRIX: glPopMatrix(); break;
        case OP_LIGHT: glLightfv(E(0), E(1), a + 2); break;
        case OP_LIGHT_MODEL: glLightModelfv(E(0), a + 1); break;
        case OP_MATERIAL: glMaterialfv(E(0), E(1), a + 2); break;
        case OP_COLOR_MATERIAL: glColorMaterial(E(0), E(1)); break;
        case OP_FOG: glFogfv(E(0), a + 1); break;
        case OP_BEGIN: glBegin(E(0)); break;
        case OP_END: glEnd(); break;
        case OP_VERTEX: glVertex4f(a[0], a[1], a[2], a[3]); break;
        case OP_COLOR: glColor4f(a[0], a[1], a[2], a[3]); break;
        case OP_NORMAL: glNormal3f(a[0], a[1], a[2]); break;
        case OP_TEXCOORD: glTexCoord4f(a[0], a[1], a[2], a[3]); break;
        case OP_BIND_TEXTURE: glBindTexture(E(0), E(1)); break;
        case OP_TEX_IMAGE: {
            int w = (int)a[2], h = (int)a[3];
            uint32_t *px = dup_pixels(c, l->blobs[(int)a[4]], (size_t)w * h);
            if (px) tex_store(c, (int)a[0], E(1), w, h, px);
            break; }
        case OP_TEX_SUB_IMAGE: {
            int lv = (int)a[0], x = (int)a[1], y = (int)a[2], w = (int)a[3], h = (int)a[4];
            if (lv < 0 || lv >= MAX_LEVELS) break;
            struct level *dst = &c->textures[c->bound_tex].lv[lv];
            if (!dst->px || x < 0 || y < 0 || w < 0 || h < 0 || w > dst->w - x || h > dst->h - y) { set_error(GL_INVALID_VALUE); break; }
            tex_sub_store(c, lv, x, y, w, h, l->blobs[(int)a[5]]);
            break; }
        case OP_TEX_PARAMETER: glTexParameterfv(E(0), E(1), a + 2); break;
        case OP_TEX_ENV: glTexEnvfv(E(0), E(1), a + 2); break;
        case OP_CALL_LIST: execute_list(c, E(0)); break;
        case OP_CALL_LIST_OFFSET: execute_list(c, c->list_base + E(0)); break;
        }
        #undef E
    }
    c->list_depth--; c->compiling = saved;
}
void glCallList(GLuint list) { CTX(); RECORD(OP_CALL_LIST, (float)list); execute_list(c, list); }
void glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {
    CTX();
    if (n < 0) { set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) {
        GLuint id;
        switch (type) {
        case GL_BYTE: id = (GLuint)((const int8_t *)lists)[i]; break;
        case GL_UNSIGNED_BYTE: id = ((const uint8_t *)lists)[i]; break;
        case GL_SHORT: id = (GLuint)((const int16_t *)lists)[i]; break;
        case GL_UNSIGNED_SHORT: id = ((const uint16_t *)lists)[i]; break;
        case GL_INT: id = (GLuint)((const int32_t *)lists)[i]; break;
        case GL_UNSIGNED_INT: id = ((const uint32_t *)lists)[i]; break;
        case GL_FLOAT: id = (GLuint)((const float *)lists)[i]; break;
        default: set_error(GL_INVALID_ENUM); return;
        }
        if (c->compiling) {
            float a[1] = { (float)id }; record(c, OP_CALL_LIST_OFFSET, 1, a);
            if (c->list_mode == GL_COMPILE) continue;
        }
        execute_list(c, c->list_base + id);
    }
}
void glListBase(GLuint base) { CTX(); c->list_base = base; }

/* ---- reading pixels ---- */
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels) {
    CTX();
    int nc = format_components(format), cs = type_size(type);
    if (!nc || (type != GL_UNSIGNED_BYTE && type != GL_FLOAT && !(format == GL_DEPTH_COMPONENT && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT)))) { set_error(GL_INVALID_ENUM); return; }
    if (w < 0 || h < 0) { set_error(GL_INVALID_VALUE); return; }
    if (c->in_begin) { set_error(GL_INVALID_OPERATION); return; }
    size_t stride = row_stride(w, c->pack_row_len, c->pack_align, nc * cs, cs);
    uint8_t *base = (uint8_t *)pixels + (size_t)c->pack_skip_rows * stride + (size_t)c->pack_skip_px * nc * cs;
    /* only the part of the rectangle inside the buffer is written; 64-bit so x + w and y + h cannot overflow */
    int j0 = (int)clampll(-(long long)y, 0, h), j1 = (int)clampll((long long)c->height - y, 0, h);
    int i0 = (int)clampll(-(long long)x, 0, w), i1 = (int)clampll((long long)c->width - x, 0, w);
    for (int j = j0; j < j1; j++) {
        int by = c->height - 1 - (y + j);          /* GL rows count up from the bottom */
        uint8_t *row = base + (size_t)j * stride;
        for (int i = i0; i < i1; i++) {
            int bx = x + i;
            size_t idx = (size_t)by * c->width + bx;
            float v[4], rgba[4];
            if (format == GL_DEPTH_COMPONENT) v[0] = c->depth[idx];
            else {
                unpack_texel(c->color[idx], rgba);
                switch (format) {
                case GL_RGB: v[0] = rgba[0]; v[1] = rgba[1]; v[2] = rgba[2]; break;
                case GL_BGR: v[0] = rgba[2]; v[1] = rgba[1]; v[2] = rgba[0]; break;
                case GL_BGRA: v[0] = rgba[2]; v[1] = rgba[1]; v[2] = rgba[0]; v[3] = rgba[3]; break;
                case GL_LUMINANCE: v[0] = clamp01(rgba[0] + rgba[1] + rgba[2]); break;
                case GL_LUMINANCE_ALPHA: v[0] = clamp01(rgba[0] + rgba[1] + rgba[2]); v[1] = rgba[3]; break;
                case GL_RED: v[0] = rgba[0]; break;
                case GL_GREEN: v[0] = rgba[1]; break;
                case GL_BLUE: v[0] = rgba[2]; break;
                case GL_ALPHA: v[0] = rgba[3]; break;
                default: cp4(v, rgba); break;
                }
            }
            uint8_t *p = row + (size_t)i * nc * cs;
            for (int k = 0; k < nc; k++, p += cs) {
                if (type == GL_FLOAT) *(float *)p = v[k];
                else if (type == GL_UNSIGNED_SHORT) *(uint16_t *)p = (uint16_t)(v[k] * 65535.0f + 0.5f);
                else if (type == GL_UNSIGNED_INT) *(uint32_t *)p = (uint32_t)((double)v[k] * 4294967295.0 + 0.5);
                else *p = (uint8_t)(v[k] * 255.0f + 0.5f);
            }
        }
    }
}

/* ---- GLU ---- */
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zn, GLdouble zf) {
    double ymax = zn * glm_tanf((float)(fovy * GLM_PI / 360.0));
    double xmax = ymax * aspect;
    glFrustum(-xmax, xmax, -ymax, ymax, zn, zf);
}
void gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez, GLdouble cx, GLdouble cy, GLdouble cz, GLdouble ux, GLdouble uy, GLdouble uz) {
    float f[3] = { (float)(cx - ex), (float)(cy - ey), (float)(cz - ez) };
    normalize3(f);
    float up[3] = { (float)ux, (float)uy, (float)uz };
    float s[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
    normalize3(s);
    float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    mat4 m; mat_identity(m);
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    glMultMatrixf(m);
    glTranslatef((float)-ex, (float)-ey, (float)-ez);
}
void gluOrtho2D(GLdouble l, GLdouble r, GLdouble b, GLdouble t) { glOrtho(l, r, b, t, -1, 1); }
static void mat_mul_d(double *out, const double *a, const double *b) {
    double r[16];
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { double s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k]; r[c * 4 + rr] = s; }
    for (int i = 0; i < 16; i++) out[i] = r[i];
}
static int invert_d(const double *m, double *out) {   /* Gauss-Jordan with partial pivoting */
    double a[4][8];
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) { a[r][k] = m[k * 4 + r]; a[r][4 + k] = r == k; }
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++) if ((a[r][col] < 0 ? -a[r][col] : a[r][col]) > (a[piv][col] < 0 ? -a[piv][col] : a[piv][col])) piv = r;
        if (a[piv][col] == 0) return 0;
        for (int k = 0; k < 8; k++) { double t = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = t; }
        double d = 1 / a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] *= d;
        for (int r = 0; r < 4; r++) if (r != col) { double f = a[r][col]; for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k]; }
    }
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) out[k * 4 + r] = a[r][4 + k];
    return 1;
}
GLint gluProject(GLdouble ox, GLdouble oy, GLdouble oz, const GLdouble *model, const GLdouble *proj, const GLint *view, GLdouble *wx, GLdouble *wy, GLdouble *wz) {
    double in[4] = { ox, oy, oz, 1 }, eye[4], clip[4];
    for (int i = 0; i < 4; i++) eye[i] = model[i] * in[0] + model[4 + i] * in[1] + model[8 + i] * in[2] + model[12 + i] * in[3];
    for (int i = 0; i < 4; i++) clip[i] = proj[i] * eye[0] + proj[4 + i] * eye[1] + proj[8 + i] * eye[2] + proj[12 + i] * eye[3];
    if (clip[3] == 0) return GL_FALSE;
    double nx = clip[0] / clip[3], ny = clip[1] / clip[3], nz = clip[2] / clip[3];
    *wx = view[0] + (nx + 1) * view[2] / 2; *wy = view[1] + (ny + 1) * view[3] / 2; *wz = (nz + 1) / 2;
    return GL_TRUE;
}
GLint gluUnProject(GLdouble wx, GLdouble wy, GLdouble wz, const GLdouble *model, const GLdouble *proj, const GLint *view, GLdouble *ox, GLdouble *oy, GLdouble *oz) {
    double pm[16], inv[16];
    mat_mul_d(pm, proj, model);
    if (!invert_d(pm, inv) || !view[2] || !view[3]) return GL_FALSE;
    double in[4] = { (wx - view[0]) * 2 / view[2] - 1, (wy - view[1]) * 2 / view[3] - 1, 2 * wz - 1, 1 }, out[4];
    for (int i = 0; i < 4; i++) out[i] = inv[i] * in[0] + inv[4 + i] * in[1] + inv[8 + i] * in[2] + inv[12 + i] * in[3];
    if (out[3] == 0) return GL_FALSE;
    *ox = out[0] / out[3]; *oy = out[1] / out[3]; *oz = out[2] / out[3];
    return GL_TRUE;
}
static int nearest_pow2(int v) { int p = 1; while (p < MAX_TEX_SIZE && p * 2 <= v) p *= 2; return (p < MAX_TEX_SIZE && p * 2 - v < v - p) ? p * 2 : p; }   /* <= MAX_TEX_SIZE, and p * 2 cannot overflow */
static uint32_t *resample(const uint32_t *src, int w, int h, int nw, int nh) {
    uint32_t *dst = GL_MALLOC((size_t)nw * nh * 4);
    if (!dst) return 0;
    for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
        /* box filter over the source footprint of this texel */
        int sx0 = (int)((long long)x * w / nw), sx1 = (int)((long long)(x + 1) * w / nw), sy0 = (int)((long long)y * h / nh), sy1 = (int)((long long)(y + 1) * h / nh);
        if (sx1 <= sx0) sx1 = sx0 + 1; if (sy1 <= sy0) sy1 = sy0 + 1;
        uint64_t sum[4] = { 0 }, cnt = 0;
        for (int sy = sy0; sy < sy1; sy++) for (int sx = sx0; sx < sx1; sx++) {
            uint32_t p = src[(size_t)sy * w + sx];
            sum[0] += p >> 24; sum[1] += (p >> 16) & 0xFF; sum[2] += (p >> 8) & 0xFF; sum[3] += p & 0xFF; cnt++;
        }
        dst[(size_t)y * nw + x] = (uint32_t)((sum[0] + cnt / 2) / cnt << 24 | (sum[1] + cnt / 2) / cnt << 16 | (sum[2] + cnt / 2) / cnt << 8 | (sum[3] + cnt / 2) / cnt);
    }
    return dst;
}
GLint gluBuild2DMipmaps(GLenum target, GLint internal, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data) {
    CTX(GLU_INVALID_VALUE);
    GLenum base = base_format(internal);
    if (target != GL_TEXTURE_2D || !base || !format_components(format) || format == GL_DEPTH_COMPONENT || (type != GL_UNSIGNED_BYTE && type != GL_FLOAT)) return GLU_INVALID_ENUM;
    if (w <= 0 || h <= 0) return GLU_INVALID_VALUE;
    uint32_t *src = unpack_image(c, w, h, format, type, data, base);
    if (!src) return GLU_OUT_OF_MEMORY;
    int nw = nearest_pow2(w), nh = nearest_pow2(h);
    if (nw > MAX_TEX_SIZE) nw = MAX_TEX_SIZE; if (nh > MAX_TEX_SIZE) nh = MAX_TEX_SIZE;
    uint32_t *lv = (nw == w && nh == h) ? src : resample(src, w, h, nw, nh);
    if (lv != src) GL_FREE(src);
    if (!lv) return GLU_OUT_OF_MEMORY;
    for (int level = 0; ; level++) {
        int last = nw == 1 && nh == 1;
        int mw = nw > 1 ? nw / 2 : 1, mh = nh > 1 ? nh / 2 : 1;
        uint32_t *next = last ? 0 : resample(lv, nw, nh, mw, mh);
        submit_tex_image(c, level, (GLenum)internal, nw, nh, lv);   /* takes ownership of lv */
        if (last) break;
        if (!next) return GLU_OUT_OF_MEMORY;
        lv = next; nw = mw; nh = mh;
    }
    return 0;
}
