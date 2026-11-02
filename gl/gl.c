/* inFlux software OpenGL. See gl.h. Compiled once for the kernel, once for user space. */
#include "gl.h"
#include "gl_port.h"   /* GL_MALLOC, GL_FREE, GL_MEMSET, GL_MEMCPY, GL_STRLEN */

#define MAX_STACK 32
#define MAX_TEXTURES 64
#define MAX_LIGHTS 2
#define MAX_VERTS 4096

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
static float fastfloor(float v) { int i = (int)v; return (float)(v < i ? i - 1 : i); }

typedef float mat4[16];   /* column-major like OpenGL */

static void mat_identity(mat4 m) { GL_MEMSET(m, 0, sizeof(mat4)); m[0] = m[5] = m[10] = m[15] = 1; }
static void mat_mul(mat4 out, const mat4 a, const mat4 b) {   /* out = a * b */
    mat4 r;
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
        r[c * 4 + rr] = s;
    }
    GL_MEMCPY(out, r, sizeof r);
}
static void mat_vec(const mat4 m, const float *v, float *out) {
    for (int i = 0; i < 4; i++) out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
}

/* ---- context ---- */
struct vertex { float pos[4]; float color[4]; float normal[3]; float tex[2]; };
struct tvertex { float x, y, z, w; float r, g, b, a; float s, t; };   /* screen-space */
struct texture { int w, h; uint32_t *px; int used; int mag_linear, min_linear, wrap_s, wrap_t; };
struct light { int enabled; float pos[4], diffuse[4], ambient[4]; };

struct gl_context {
    int width, height;
    uint32_t *color;
    float *depth;
    float clear_color[4]; float clear_depth;
    int vp_x, vp_y, vp_w, vp_h;
    int depth_test, depth_write; GLenum depth_func;
    int cull, lighting, blend, tex2d, color_material;
    GLenum cull_face, front_face, shade, blend_src, blend_dst, polygon_mode;
    float point_size;
    GLenum matrix_mode;
    mat4 modelview[MAX_STACK], projection[MAX_STACK]; int mv_top, pr_top;
    struct light lights[MAX_LIGHTS]; float ambient_model[4];
    float mat_ambient[4], mat_diffuse[4];
    /* immediate mode */
    int in_begin; GLenum prim_mode; struct vertex cur; struct vertex *verts; int nverts;
    struct texture textures[MAX_TEXTURES]; GLuint bound_tex; GLenum tex_env;
    GLenum error;
};

static GLcontext *cur;
static char renderer_name[128] = "inFlux Software Rasterizer";

GLcontext *glxCreateContext(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    GLcontext *c = GL_MALLOC(sizeof *c);
    if (!c) return 0;
    GL_MEMSET(c, 0, sizeof *c);
    c->width = w; c->height = h;
    c->color = GL_MALLOC((size_t)w * h * 4);
    c->depth = GL_MALLOC((size_t)w * h * sizeof(float));
    c->verts = GL_MALLOC(MAX_VERTS * sizeof(struct vertex));
    if (!c->color || !c->depth || !c->verts) { glxDestroyContext(c); return 0; }
    c->clear_depth = 1.0f;
    c->vp_w = w; c->vp_h = h;
    c->depth_func = GL_LESS; c->depth_write = 1;
    c->cull_face = GL_BACK; c->front_face = GL_CCW; c->shade = GL_SMOOTH; c->polygon_mode = GL_FILL;
    c->blend_src = GL_SRC_ALPHA; c->blend_dst = GL_ONE_MINUS_SRC_ALPHA;
    c->point_size = 1.0f;
    c->matrix_mode = GL_MODELVIEW;
    mat_identity(c->modelview[0]); mat_identity(c->projection[0]);
    c->cur.color[0] = c->cur.color[1] = c->cur.color[2] = c->cur.color[3] = 1;
    c->cur.normal[2] = 1;
    for (int i = 0; i < MAX_LIGHTS; i++) {
        c->lights[i].pos[2] = 1; c->lights[i].pos[3] = 0;
        c->lights[i].diffuse[0] = c->lights[i].diffuse[1] = c->lights[i].diffuse[2] = c->lights[i].diffuse[3] = i == 0 ? 1 : 0;
        c->lights[i].ambient[3] = 1;
    }
    c->ambient_model[0] = c->ambient_model[1] = c->ambient_model[2] = 0.2f; c->ambient_model[3] = 1;
    c->mat_ambient[0] = c->mat_ambient[1] = c->mat_ambient[2] = 0.2f; c->mat_ambient[3] = 1;
    c->mat_diffuse[0] = c->mat_diffuse[1] = c->mat_diffuse[2] = 0.8f; c->mat_diffuse[3] = 1;
    c->tex_env = GL_MODULATE;
    c->textures[0].used = 1;
    return c;
}
void glxDestroyContext(GLcontext *c) {
    if (!c) return;
    for (int i = 0; i < MAX_TEXTURES; i++) if (c->textures[i].px) GL_FREE(c->textures[i].px);
    if (c->color) GL_FREE(c->color);
    if (c->depth) GL_FREE(c->depth);
    if (c->verts) GL_FREE(c->verts);
    if (cur == c) cur = 0;
    GL_FREE(c);
}
void glxMakeCurrent(GLcontext *c) { cur = c; }
uint32_t *glxGetPixels(GLcontext *c) { return c ? c->color : 0; }
int glxGetWidth(GLcontext *c) { return c ? c->width : 0; }
int glxGetHeight(GLcontext *c) { return c ? c->height : 0; }
void glxSetRendererName(const char *n) { size_t l = GL_STRLEN(n); if (l > sizeof renderer_name - 1) l = sizeof renderer_name - 1; GL_MEMCPY(renderer_name, n, l); renderer_name[l] = 0; }

static void set_error(GLenum e) { if (cur && cur->error == GL_NO_ERROR) cur->error = e; }
#define CTX(ret) GLcontext *c = cur; if (!c) return ret

/* ---- state ---- */
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { CTX(); c->clear_color[0] = r; c->clear_color[1] = g; c->clear_color[2] = b; c->clear_color[3] = a; }
void glClearDepth(GLdouble d) { CTX(); c->clear_depth = (float)d; }
static uint32_t pack_color(float r, float g, float b, float a) {
    return ((uint32_t)(clamp01(a) * 255.0f + 0.5f) << 24) | ((uint32_t)(clamp01(r) * 255.0f + 0.5f) << 16) | ((uint32_t)(clamp01(g) * 255.0f + 0.5f) << 8) | (uint32_t)(clamp01(b) * 255.0f + 0.5f);
}
void glClear(GLbitfield mask) {
    CTX();
    size_t n = (size_t)c->width * c->height;
    if (mask & GL_COLOR_BUFFER_BIT) { uint32_t col = pack_color(c->clear_color[0], c->clear_color[1], c->clear_color[2], c->clear_color[3]); for (size_t i = 0; i < n; i++) c->color[i] = col; }
    if (mask & GL_DEPTH_BUFFER_BIT) for (size_t i = 0; i < n; i++) c->depth[i] = c->clear_depth;
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { CTX(); c->vp_x = x; c->vp_y = y; c->vp_w = w; c->vp_h = h; }
static int *cap_ptr(GLcontext *c, GLenum cap) {
    switch (cap) {
    case GL_DEPTH_TEST: return &c->depth_test; case GL_CULL_FACE: return &c->cull; case GL_LIGHTING: return &c->lighting;
    case GL_BLEND: return &c->blend; case GL_TEXTURE_2D: return &c->tex2d; case GL_COLOR_MATERIAL: return &c->color_material;
    case GL_LIGHT0: return &c->lights[0].enabled; case GL_LIGHT1: return &c->lights[1].enabled;
    }
    return 0;
}
void glEnable(GLenum cap) { CTX(); int *p = cap_ptr(c, cap); if (p) *p = 1; else set_error(GL_INVALID_ENUM); }
void glDisable(GLenum cap) { CTX(); int *p = cap_ptr(c, cap); if (p) *p = 0; else set_error(GL_INVALID_ENUM); }
GLboolean glIsEnabled(GLenum cap) { CTX(0); int *p = cap_ptr(c, cap); return p ? (GLboolean)*p : 0; }
void glDepthFunc(GLenum f) { CTX(); c->depth_func = f; }
void glDepthMask(GLboolean f) { CTX(); c->depth_write = f; }
void glCullFace(GLenum m) { CTX(); c->cull_face = m; }
void glFrontFace(GLenum m) { CTX(); c->front_face = m; }
void glShadeModel(GLenum m) { CTX(); c->shade = m; }
void glBlendFunc(GLenum s, GLenum d) { CTX(); c->blend_src = s; c->blend_dst = d; }
void glPolygonMode(GLenum face, GLenum mode) { CTX(); c->polygon_mode = mode; }
void glPointSize(GLfloat s) { CTX(); c->point_size = s; }
void glLineWidth(GLfloat w) { (void)w; }
void glFlush(void) {}
void glFinish(void) {}
GLenum glGetError(void) { CTX(GL_NO_ERROR); GLenum e = c->error; c->error = GL_NO_ERROR; return e; }
const GLubyte *glGetString(GLenum name) {
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"inFlux";
    case GL_RENDERER: return (const GLubyte *)renderer_name;
    case GL_VERSION: return (const GLubyte *)"1.1 inFlux software";
    case GL_EXTENSIONS: return (const GLubyte *)"";
    }
    return (const GLubyte *)"";
}
void glGetIntegerv(GLenum pname, GLint *params) {
    CTX();
    if (pname == GL_MAX_TEXTURE_SIZE) params[0] = 1024;
    else if (pname == GL_VIEWPORT) { params[0] = c->vp_x; params[1] = c->vp_y; params[2] = c->vp_w; params[3] = c->vp_h; }
}
void glGetFloatv(GLenum pname, GLfloat *params) { CTX(); if (pname == GL_MODELVIEW) GL_MEMCPY(params, c->modelview[c->mv_top], sizeof(mat4)); else if (pname == GL_PROJECTION) GL_MEMCPY(params, c->projection[c->pr_top], sizeof(mat4)); }

/* ---- matrices ---- */
static float *top_matrix(GLcontext *c) { return c->matrix_mode == GL_PROJECTION ? c->projection[c->pr_top] : c->modelview[c->mv_top]; }
void glMatrixMode(GLenum m) { CTX(); c->matrix_mode = m; }
void glLoadIdentity(void) { CTX(); mat_identity(top_matrix(c)); }
void glLoadMatrixf(const GLfloat *m) { CTX(); GL_MEMCPY(top_matrix(c), m, sizeof(mat4)); }
void glMultMatrixf(const GLfloat *m) { CTX(); float *t = top_matrix(c); mat_mul(t, t, m); }
void glPushMatrix(void) {
    CTX();
    if (c->matrix_mode == GL_PROJECTION) { if (c->pr_top + 1 >= MAX_STACK) { set_error(GL_STACK_OVERFLOW); return; } GL_MEMCPY(c->projection[c->pr_top + 1], c->projection[c->pr_top], sizeof(mat4)); c->pr_top++; }
    else { if (c->mv_top + 1 >= MAX_STACK) { set_error(GL_STACK_OVERFLOW); return; } GL_MEMCPY(c->modelview[c->mv_top + 1], c->modelview[c->mv_top], sizeof(mat4)); c->mv_top++; }
}
void glPopMatrix(void) {
    CTX();
    if (c->matrix_mode == GL_PROJECTION) { if (c->pr_top == 0) { set_error(GL_STACK_UNDERFLOW); return; } c->pr_top--; }
    else { if (c->mv_top == 0) { set_error(GL_STACK_UNDERFLOW); return; } c->mv_top--; }
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
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    mat4 m; mat_identity(m);
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n));
    glMultMatrixf(m);
}
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    mat4 m; GL_MEMSET(m, 0, sizeof m);
    m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b)); m[10] = (float)(-(f + n) / (f - n)); m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    glMultMatrixf(m);
}
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zn, GLdouble zf) {
    double ymax = zn * glm_tanf((float)(fovy * GLM_PI / 360.0));
    double xmax = ymax * aspect;
    glFrustum(-xmax, xmax, -ymax, ymax, zn, zf);
}
void gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez, GLdouble cx, GLdouble cy, GLdouble cz, GLdouble ux, GLdouble uy, GLdouble uz) {
    float f[3] = { (float)(cx - ex), (float)(cy - ey), (float)(cz - ez) };
    float fl = glm_sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]); if (fl > 0) { f[0] /= fl; f[1] /= fl; f[2] /= fl; }
    float up[3] = { (float)ux, (float)uy, (float)uz };
    float s[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
    float sl = glm_sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]); if (sl > 0) { s[0] /= sl; s[1] /= sl; s[2] /= sl; }
    float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    mat4 m; mat_identity(m);
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    glMultMatrixf(m);
    glTranslatef((float)-ex, (float)-ey, (float)-ez);
}

/* ---- lighting ---- */
void glLightfv(GLenum light, GLenum pname, const GLfloat *p) {
    CTX();
    int i = light - GL_LIGHT0; if (i < 0 || i >= MAX_LIGHTS) { set_error(GL_INVALID_ENUM); return; }
    struct light *l = &c->lights[i];
    if (pname == GL_POSITION) { float v[4] = { p[0], p[1], p[2], p[3] }; mat_vec(c->modelview[c->mv_top], v, l->pos); }   /* stored in eye space */
    else if (pname == GL_DIFFUSE) GL_MEMCPY(l->diffuse, p, 16);
    else if (pname == GL_AMBIENT) GL_MEMCPY(l->ambient, p, 16);
}
void glLightModelfv(GLenum pname, const GLfloat *p) { CTX(); if (pname == GL_LIGHT_MODEL_AMBIENT) GL_MEMCPY(c->ambient_model, p, 16); }
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *p) { CTX(); if (pname == GL_AMBIENT) GL_MEMCPY(c->mat_ambient, p, 16); else if (pname == GL_DIFFUSE) GL_MEMCPY(c->mat_diffuse, p, 16); }
void glColorMaterial(GLenum face, GLenum mode) { (void)face; (void)mode; }

/* ---- textures ---- */
void glGenTextures(GLsizei n, GLuint *out) { CTX(); for (GLsizei k = 0; k < n; k++) { out[k] = 0; for (GLuint i = 1; i < MAX_TEXTURES; i++) if (!c->textures[i].used) { c->textures[i].used = 1; c->textures[i].mag_linear = c->textures[i].min_linear = 1; c->textures[i].wrap_s = c->textures[i].wrap_t = GL_REPEAT; out[k] = i; break; } } }
void glDeleteTextures(GLsizei n, const GLuint *t) { CTX(); for (GLsizei k = 0; k < n; k++) if (t[k] < MAX_TEXTURES && t[k]) { if (c->textures[t[k]].px) GL_FREE(c->textures[t[k]].px); GL_MEMSET(&c->textures[t[k]], 0, sizeof(struct texture)); } }
void glBindTexture(GLenum target, GLuint t) { CTX(); if (t < MAX_TEXTURES) c->bound_tex = t; }
void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const GLvoid *pixels) {
    CTX();
    if (level != 0 || w <= 0 || h <= 0 || w > 1024 || h > 1024) { if (level != 0) return; set_error(GL_INVALID_VALUE); return; }
    struct texture *t = &c->textures[c->bound_tex];
    if (t->px) GL_FREE(t->px);
    t->px = GL_MALLOC((size_t)w * h * 4); t->w = w; t->h = h;
    const GLubyte *s = pixels;
    int bpp = format == GL_RGB ? 3 : 4;
    for (int i = 0; i < w * h; i++) {
        GLubyte r = s ? s[i * bpp] : 255, g = s ? s[i * bpp + 1] : 255, b = s ? s[i * bpp + 2] : 255, a = (s && bpp == 4) ? s[i * 4 + 3] : 255;
        t->px[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
}
void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    CTX(); struct texture *t = &c->textures[c->bound_tex];
    if (pname == GL_TEXTURE_MAG_FILTER) t->mag_linear = param == GL_LINEAR; else if (pname == GL_TEXTURE_MIN_FILTER) t->min_linear = param != GL_NEAREST;
    else if (pname == GL_TEXTURE_WRAP_S) t->wrap_s = param; else if (pname == GL_TEXTURE_WRAP_T) t->wrap_t = param;
}
void glTexEnvi(GLenum target, GLenum pname, GLint param) { CTX(); if (pname == GL_TEXTURE_ENV_MODE) c->tex_env = param; }

static void tex_sample(struct texture *t, float s, float tt, float *out) {
    if (!t->px) { out[0] = out[1] = out[2] = out[3] = 1; return; }
    if (t->wrap_s == GL_REPEAT) s -= fastfloor(s); else s = clamp01(s);
    if (t->wrap_t == GL_REPEAT) tt -= fastfloor(tt); else tt = clamp01(tt);
    float fx = s * t->w - 0.5f, fy = tt * t->h - 0.5f;
    int x0 = (int)fastfloor(fx), y0 = (int)fastfloor(fy);
    float ax = fx - x0, ay = fy - y0;
    if (!t->mag_linear) { ax = ay = 0; x0 = (int)(s * t->w); y0 = (int)(tt * t->h); }
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = ((x0 % t->w) + t->w) % t->w; x1 = ((x1 % t->w) + t->w) % t->w; y0 = ((y0 % t->h) + t->h) % t->h; y1 = ((y1 % t->h) + t->h) % t->h;
    uint32_t p00 = t->px[y0 * t->w + x0], p10 = t->px[y0 * t->w + x1], p01 = t->px[y1 * t->w + x0], p11 = t->px[y1 * t->w + x1];
    for (int ch = 0; ch < 4; ch++) {
        int sh = ch == 3 ? 24 : 16 - ch * 8;
        float v00 = (p00 >> sh) & 0xFF, v10 = (p10 >> sh) & 0xFF, v01 = (p01 >> sh) & 0xFF, v11 = (p11 >> sh) & 0xFF;
        float top = v00 + (v10 - v00) * ax, bot = v01 + (v11 - v01) * ax;
        out[ch] = (top + (bot - top) * ay) * (1.0f / 255.0f);
    }
}

/* ---- vertex processing ---- */
static void light_vertex(GLcontext *c, const float *eye_pos, const float *eye_nrm, const float *base, float *out) {
    float r = c->ambient_model[0] * base[0], g = c->ambient_model[1] * base[1], b = c->ambient_model[2] * base[2];
    for (int i = 0; i < MAX_LIGHTS; i++) {
        struct light *l = &c->lights[i]; if (!l->enabled) continue;
        float L[3];
        if (l->pos[3] == 0) { L[0] = l->pos[0]; L[1] = l->pos[1]; L[2] = l->pos[2]; }
        else { L[0] = l->pos[0] - eye_pos[0]; L[1] = l->pos[1] - eye_pos[1]; L[2] = l->pos[2] - eye_pos[2]; }
        float ll = glm_sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]); if (ll > 0) { L[0] /= ll; L[1] /= ll; L[2] /= ll; }
        float nd = eye_nrm[0] * L[0] + eye_nrm[1] * L[1] + eye_nrm[2] * L[2]; if (nd < 0) nd = 0;
        r += l->ambient[0] * base[0] + nd * l->diffuse[0] * base[0];
        g += l->ambient[1] * base[1] + nd * l->diffuse[1] * base[1];
        b += l->ambient[2] * base[2] + nd * l->diffuse[2] * base[2];
    }
    out[0] = clamp01(r); out[1] = clamp01(g); out[2] = clamp01(b); out[3] = base[3];
}

/* transform an object-space vertex to clip space (+ lit color) */
static void transform_vertex(GLcontext *c, const struct vertex *v, float *clip, float *color) {
    float eye[4]; mat_vec(c->modelview[c->mv_top], v->pos, eye);
    mat_vec(c->projection[c->pr_top], eye, clip);
    if (c->lighting) {
        /* normal: use the upper 3x3 of the modelview (assumes uniform scale) */
        const float *m = c->modelview[c->mv_top];
        float n[3] = { m[0] * v->normal[0] + m[4] * v->normal[1] + m[8] * v->normal[2], m[1] * v->normal[0] + m[5] * v->normal[1] + m[9] * v->normal[2], m[2] * v->normal[0] + m[6] * v->normal[1] + m[10] * v->normal[2] };
        float nl = glm_sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (nl > 0) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
        light_vertex(c, eye, n, v->color, color);
    } else GL_MEMCPY(color, v->color, 16);
}

/* ---- rasterisation ---- */
static inline int depth_pass(GLcontext *c, float z, float old) {
    switch (c->depth_func) {
    case GL_NEVER: return 0; case GL_LESS: return z < old; case GL_EQUAL: return z == old; case GL_LEQUAL: return z <= old;
    case GL_GREATER: return z > old; case GL_NOTEQUAL: return z != old; case GL_GEQUAL: return z >= old; default: return 1;
    }
}
static inline void write_pixel(GLcontext *c, int x, int y, float z, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= c->width || y >= c->height) return;
    int idx = y * c->width + x;
    if (c->depth_test) { if (!depth_pass(c, z, c->depth[idx])) return; if (c->depth_write) c->depth[idx] = z; }
    if (c->blend && a < 1.0f) {
        uint32_t d = c->color[idx];
        float dr = ((d >> 16) & 0xFF) / 255.0f, dg = ((d >> 8) & 0xFF) / 255.0f, db = (d & 0xFF) / 255.0f;
        float sa = c->blend_src == GL_ONE ? 1 : c->blend_src == GL_ZERO ? 0 : a;
        float da = c->blend_dst == GL_ONE ? 1 : c->blend_dst == GL_ZERO ? 0 : c->blend_dst == GL_SRC_ALPHA ? a : 1 - a;
        r = r * sa + dr * da; g = g * sa + dg * da; b = b * sa + db * da; a = 1;
    }
    c->color[idx] = pack_color(r, g, b, a);
}

static void draw_point(GLcontext *c, const struct tvertex *v) {
    int sz = (int)(c->point_size + 0.5f); if (sz < 1) sz = 1;
    int x0 = (int)(v->x - sz / 2.0f), y0 = (int)(v->y - sz / 2.0f);
    for (int y = y0; y < y0 + sz; y++) for (int x = x0; x < x0 + sz; x++) write_pixel(c, x, y, v->z, v->r, v->g, v->b, v->a);
}
static void draw_line(GLcontext *c, const struct tvertex *a, const struct tvertex *b) {
    float dx = b->x - a->x, dy = b->y - a->y;
    float steps = fabsf_(dx) > fabsf_(dy) ? fabsf_(dx) : fabsf_(dy);
    int n = (int)steps + 1;
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / n : 0;
        float r = a->r + (b->r - a->r) * t, g = a->g + (b->g - a->g) * t, bb = a->b + (b->b - a->b) * t, al = a->a + (b->a - a->a) * t;
        write_pixel(c, (int)(a->x + dx * t + 0.5f), (int)(a->y + dy * t + 0.5f), a->z + (b->z - a->z) * t, r, g, bb, al);
    }
}
static void draw_triangle(GLcontext *c, const struct tvertex *v0, const struct tvertex *v1, const struct tvertex *v2, int flat) {
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0) return;
    /* face culling: screen y points down, so CCW in GL becomes negative area here */
    int front_ccw = c->front_face == GL_CCW;
    int is_front = front_ccw ? area < 0 : area > 0;
    if (c->cull) { if ((c->cull_face == GL_BACK && !is_front) || (c->cull_face == GL_FRONT && is_front) || c->cull_face == GL_FRONT_AND_BACK) return; }
    if (c->polygon_mode == GL_LINE) { draw_line(c, v0, v1); draw_line(c, v1, v2); draw_line(c, v2, v0); return; }
    if (c->polygon_mode == GL_POINT) { draw_point(c, v0); draw_point(c, v1); draw_point(c, v2); return; }
    float minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    if (v1->x < minx) minx = v1->x; if (v1->x > maxx) maxx = v1->x; if (v2->x < minx) minx = v2->x; if (v2->x > maxx) maxx = v2->x;
    if (v1->y < miny) miny = v1->y; if (v1->y > maxy) maxy = v1->y; if (v2->y < miny) miny = v2->y; if (v2->y > maxy) maxy = v2->y;
    int x0 = (int)minx, x1 = (int)maxx + 1, y0 = (int)miny, y1 = (int)maxy + 1;
    int cx0 = c->vp_x < 0 ? 0 : c->vp_x, cy0 = c->height - c->vp_y - c->vp_h, cx1 = c->vp_x + c->vp_w, cy1 = c->height - c->vp_y;
    if (cy0 < 0) cy0 = 0; if (cx1 > c->width) cx1 = c->width; if (cy1 > c->height) cy1 = c->height;
    if (x0 < cx0) x0 = cx0; if (y0 < cy0) y0 = cy0; if (x1 > cx1) x1 = cx1; if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return;
    float inv_area = 1.0f / area;
    struct texture *tex = (c->tex2d && c->bound_tex) ? &c->textures[c->bound_tex] : 0;
    /* perspective-correct attribute interpolation uses 1/w */
    float iw0 = 1.0f / v0->w, iw1 = 1.0f / v1->w, iw2 = 1.0f / v2->w;
    for (int y = y0; y < y1; y++) {
        float py = y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f;
            float w0 = ((v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py)) * inv_area;
            float w1 = ((v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
            float r, g, b, a;
            if (flat) { r = v2->r; g = v2->g; b = v2->b; a = v2->a; }
            else { r = w0 * v0->r + w1 * v1->r + w2 * v2->r; g = w0 * v0->g + w1 * v1->g + w2 * v2->g; b = w0 * v0->b + w1 * v1->b + w2 * v2->b; a = w0 * v0->a + w1 * v1->a + w2 * v2->a; }
            if (tex) {
                float pw = w0 * iw0 + w1 * iw1 + w2 * iw2;
                float s = (w0 * v0->s * iw0 + w1 * v1->s * iw1 + w2 * v2->s * iw2) / pw;
                float t = (w0 * v0->t * iw0 + w1 * v1->t * iw1 + w2 * v2->t * iw2) / pw;
                float tc[4]; tex_sample(tex, s, t, tc);
                if (c->tex_env == GL_REPLACE) { r = tc[0]; g = tc[1]; b = tc[2]; a = tc[3]; }
                else { r *= tc[0]; g *= tc[1]; b *= tc[2]; a *= tc[3]; }
            }
            write_pixel(c, x, y, z, r, g, b, a);
        }
    }
}

/* clip-space -> screen-space */
static int to_screen(GLcontext *c, const float *clip, const float *col, const float *tex, struct tvertex *out) {
    float w = clip[3];
    if (w <= 1e-6f) return 0;
    float iw = 1.0f / w;
    float nx = clip[0] * iw, ny = clip[1] * iw, nz = clip[2] * iw;
    out->x = c->vp_x + (nx + 1) * 0.5f * c->vp_w;
    out->y = c->height - (c->vp_y + (ny + 1) * 0.5f * c->vp_h);
    out->z = (nz + 1) * 0.5f; out->w = w;
    out->r = col[0]; out->g = col[1]; out->b = col[2]; out->a = col[3];
    out->s = tex[0]; out->t = tex[1];
    return 1;
}

/* near-plane clipping of a triangle in clip space (w > epsilon), then draw */
struct cvert { float clip[4], col[4], tex[2]; };
static void emit_triangle(GLcontext *c, struct cvert *a, struct cvert *b, struct cvert *d, int flat) {
    struct cvert *in[3] = { a, b, d }; struct cvert out[4]; int n = 0;
    const float eps = 1e-3f;
    for (int i = 0; i < 3; i++) {
        struct cvert *p = in[i], *q = in[(i + 1) % 3];
        float dp = p->clip[3] - eps, dq = q->clip[3] - eps;
        if (dp >= 0) out[n++] = *p;
        if ((dp >= 0) != (dq >= 0)) {
            float t = dp / (dp - dq);
            struct cvert m;
            for (int k = 0; k < 4; k++) { m.clip[k] = p->clip[k] + (q->clip[k] - p->clip[k]) * t; m.col[k] = p->col[k] + (q->col[k] - p->col[k]) * t; }
            m.tex[0] = p->tex[0] + (q->tex[0] - p->tex[0]) * t; m.tex[1] = p->tex[1] + (q->tex[1] - p->tex[1]) * t;
            out[n++] = m;
        }
    }
    if (n < 3) return;
    struct tvertex sv[4];
    for (int i = 0; i < n; i++) if (!to_screen(c, out[i].clip, out[i].col, out[i].tex, &sv[i])) return;
    draw_triangle(c, &sv[0], &sv[1], &sv[2], flat);
    if (n == 4) draw_triangle(c, &sv[0], &sv[2], &sv[3], flat);
}
static void emit_line(GLcontext *c, struct cvert *a, struct cvert *b) {
    const float eps = 1e-3f;
    struct cvert p = *a, q = *b;
    if (p.clip[3] < eps && q.clip[3] < eps) return;
    if (p.clip[3] < eps || q.clip[3] < eps) {
        struct cvert *in = p.clip[3] < eps ? &q : &p, *o = p.clip[3] < eps ? &p : &q;
        float t = (in->clip[3] - eps) / (in->clip[3] - o->clip[3]);
        for (int k = 0; k < 4; k++) { o->clip[k] = in->clip[k] + (o->clip[k] - in->clip[k]) * t; o->col[k] = in->col[k] + (o->col[k] - in->col[k]) * t; }
    }
    struct tvertex s0, s1;
    if (to_screen(c, p.clip, p.col, p.tex, &s0) && to_screen(c, q.clip, q.col, q.tex, &s1)) draw_line(c, &s0, &s1);
}

static void flush_primitives(GLcontext *c) {
    int n = c->nverts;
    if (!n) return;
    struct cvert *cv = GL_MALLOC(n * sizeof(struct cvert));
    if (!cv) return;
    for (int i = 0; i < n; i++) { transform_vertex(c, &c->verts[i], cv[i].clip, cv[i].col); cv[i].tex[0] = c->verts[i].tex[0]; cv[i].tex[1] = c->verts[i].tex[1]; }
    int flat = c->shade == GL_FLAT;
    switch (c->prim_mode) {
    case GL_POINTS: for (int i = 0; i < n; i++) { struct tvertex s; if (to_screen(c, cv[i].clip, cv[i].col, cv[i].tex, &s)) draw_point(c, &s); } break;
    case GL_LINES: for (int i = 0; i + 1 < n; i += 2) emit_line(c, &cv[i], &cv[i + 1]); break;
    case GL_LINE_STRIP: for (int i = 0; i + 1 < n; i++) emit_line(c, &cv[i], &cv[i + 1]); break;
    case GL_LINE_LOOP: for (int i = 0; i < n; i++) emit_line(c, &cv[i], &cv[(i + 1) % n]); break;
    case GL_TRIANGLES: for (int i = 0; i + 2 < n; i += 3) emit_triangle(c, &cv[i], &cv[i + 1], &cv[i + 2], flat); break;
    case GL_TRIANGLE_STRIP: for (int i = 0; i + 2 < n; i++) { if (i & 1) emit_triangle(c, &cv[i + 1], &cv[i], &cv[i + 2], flat); else emit_triangle(c, &cv[i], &cv[i + 1], &cv[i + 2], flat); } break;
    case GL_TRIANGLE_FAN: case GL_POLYGON: for (int i = 1; i + 1 < n; i++) emit_triangle(c, &cv[0], &cv[i], &cv[i + 1], flat); break;
    case GL_QUADS: for (int i = 0; i + 3 < n; i += 4) { emit_triangle(c, &cv[i], &cv[i + 1], &cv[i + 2], flat); emit_triangle(c, &cv[i], &cv[i + 2], &cv[i + 3], flat); } break;
    case GL_QUAD_STRIP: for (int i = 0; i + 3 < n; i += 2) { emit_triangle(c, &cv[i], &cv[i + 1], &cv[i + 3], flat); emit_triangle(c, &cv[i], &cv[i + 3], &cv[i + 2], flat); } break;
    }
    GL_FREE(cv);
    c->nverts = 0;
}

/* ---- immediate mode ---- */
void glBegin(GLenum mode) { CTX(); if (c->in_begin) { set_error(GL_INVALID_OPERATION); return; } c->in_begin = 1; c->prim_mode = mode; c->nverts = 0; }
void glEnd(void) { CTX(); if (!c->in_begin) { set_error(GL_INVALID_OPERATION); return; } flush_primitives(c); c->in_begin = 0; }
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    CTX(); if (!c->in_begin) { set_error(GL_INVALID_OPERATION); return; }
    if (c->nverts >= MAX_VERTS) { if (c->prim_mode == GL_TRIANGLES || c->prim_mode == GL_QUADS || c->prim_mode == GL_LINES || c->prim_mode == GL_POINTS) flush_primitives(c); else return; }
    struct vertex *v = &c->verts[c->nverts++];
    *v = c->cur; v->pos[0] = x; v->pos[1] = y; v->pos[2] = z; v->pos[3] = w;
}
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { glVertex4f(x, y, z, 1); }
void glVertex2f(GLfloat x, GLfloat y) { glVertex4f(x, y, 0, 1); }
void glVertex3fv(const GLfloat *v) { glVertex4f(v[0], v[1], v[2], 1); }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { CTX(); c->cur.color[0] = r; c->cur.color[1] = g; c->cur.color[2] = b; c->cur.color[3] = a; }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1); }
void glColor3fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], 1); }
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1); }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { CTX(); c->cur.normal[0] = x; c->cur.normal[1] = y; c->cur.normal[2] = z; }
void glNormal3fv(const GLfloat *n) { glNormal3f(n[0], n[1], n[2]); }
void glTexCoord2f(GLfloat s, GLfloat t) { CTX(); c->cur.tex[0] = s; c->cur.tex[1] = t; }
