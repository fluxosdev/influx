/*
 * inFlux OpenGL: a fixed-function OpenGL 1.1 implementation with a software
 * rasteriser (plus a few 1.2-1.4 conveniences: BGRA pixels, CLAMP_TO_EDGE,
 * MIRRORED_REPEAT, GL_ADD, glDrawRangeElements). The same implementation runs
 * inside the kernel (desktop apps) and in user programs (link with libc).
 * Contexts render into a 32-bit ARGB framebuffer.
 *
 * Not implemented: stencil and accumulation buffers, glDrawPixels/glBitmap/
 * glRasterPos, 1D/3D textures, texgen, user clip planes, selection/feedback,
 * evaluators, glPushAttrib, line stipple and antialiasing.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned char GLubyte;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort;
typedef unsigned char GLboolean;
typedef void GLvoid;
typedef float GLclampf;
typedef double GLclampd;
typedef unsigned int GLbitfield;

#define GL_FALSE 0
#define GL_TRUE 1
/* primitives */
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_LOOP 0x0002
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_QUADS 0x0007
#define GL_QUAD_STRIP 0x0008
#define GL_POLYGON 0x0009
/* comparison functions */
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
/* blend factors */
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
/* faces */
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901
/* capabilities */
#define GL_POINT_SMOOTH 0x0B10
#define GL_LINE_SMOOTH 0x0B20
#define GL_LINE_STIPPLE 0x0B24
#define GL_POLYGON_SMOOTH 0x0B41
#define GL_CULL_FACE 0x0B44
#define GL_LIGHTING 0x0B50
#define GL_COLOR_MATERIAL 0x0B57
#define GL_FOG 0x0B60
#define GL_DEPTH_TEST 0x0B71
#define GL_NORMALIZE 0x0BA1
#define GL_ALPHA_TEST 0x0BC0
#define GL_DITHER 0x0BD0
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_2D 0x0DE1
#define GL_POLYGON_OFFSET_POINT 0x2A01
#define GL_POLYGON_OFFSET_LINE 0x2A02
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_RESCALE_NORMAL 0x803A
/* client arrays */
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
/* data types */
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_DOUBLE 0x140A
/* lighting */
#define GL_LIGHT0 0x4000
#define GL_LIGHT1 0x4001
#define GL_LIGHT2 0x4002
#define GL_LIGHT3 0x4003
#define GL_LIGHT4 0x4004
#define GL_LIGHT5 0x4005
#define GL_LIGHT6 0x4006
#define GL_LIGHT7 0x4007
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT 0x1205
#define GL_SPOT_CUTOFF 0x1206
#define GL_CONSTANT_ATTENUATION 0x1207
#define GL_LINEAR_ATTENUATION 0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209
#define GL_EMISSION 0x1600
#define GL_SHININESS 0x1601
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_COLOR_MATERIAL_FACE 0x0B55
#define GL_COLOR_MATERIAL_PARAMETER 0x0B56
#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01
/* fog */
#define GL_FOG_INDEX 0x0B61
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_EXP 0x0800
#define GL_EXP2 0x0801
/* matrices */
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702
/* buffers */
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_ACCUM_BUFFER_BIT 0x00000200
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000
/* pixel formats */
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RED 0x1903
#define GL_GREEN 0x1904
#define GL_BLUE 0x1905
#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_BGR 0x80E0
#define GL_BGRA 0x80E1
#define GL_INTENSITY 0x8049
#define GL_ALPHA8 0x803C
#define GL_LUMINANCE8 0x8040
#define GL_LUMINANCE8_ALPHA8 0x8045
#define GL_INTENSITY8 0x804B
#define GL_RGB5 0x8050
#define GL_RGB8 0x8051
#define GL_RGBA4 0x8056
#define GL_RGB5_A1 0x8057
#define GL_RGBA8 0x8058
/* pixel storage */
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#define GL_PACK_ALIGNMENT 0x0D05
/* textures */
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_BORDER 0x1005
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_CLAMP 0x2900
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_ENV_COLOR 0x2201
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_REPLACE 0x1E01
#define GL_ADD 0x0104
/* polygon modes */
#define GL_POINT 0x1B00
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02
/* display lists */
#define GL_COMPILE 0x1300
#define GL_COMPILE_AND_EXECUTE 0x1301
#define GL_LIST_MODE 0x0B30
#define GL_MAX_LIST_NESTING 0x0B31
#define GL_LIST_BASE 0x0B32
#define GL_LIST_INDEX 0x0B33
/* hints */
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_POINT_SMOOTH_HINT 0x0C51
#define GL_LINE_SMOOTH_HINT 0x0C52
#define GL_POLYGON_SMOOTH_HINT 0x0C53
#define GL_FOG_HINT 0x0C54
#define GL_DONT_CARE 0x1100
#define GL_FASTEST 0x1101
#define GL_NICEST 0x1102
/* strings */
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
/* errors */
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_STACK_OVERFLOW 0x0503
#define GL_STACK_UNDERFLOW 0x0504
#define GL_OUT_OF_MEMORY 0x0505
/* state queries */
#define GL_CURRENT_COLOR 0x0B00
#define GL_CURRENT_NORMAL 0x0B02
#define GL_CURRENT_TEXTURE_COORDS 0x0B03
#define GL_POINT_SIZE 0x0B11
#define GL_LINE_WIDTH 0x0B21
#define GL_POLYGON_MODE 0x0B40
#define GL_CULL_FACE_MODE 0x0B45
#define GL_FRONT_FACE 0x0B46
#define GL_SHADE_MODEL 0x0B54
#define GL_DEPTH_RANGE 0x0B70
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_DEPTH_CLEAR_VALUE 0x0B73
#define GL_DEPTH_FUNC 0x0B74
#define GL_MATRIX_MODE 0x0BA0
#define GL_VIEWPORT 0x0BA2
#define GL_MODELVIEW_STACK_DEPTH 0x0BA3
#define GL_PROJECTION_STACK_DEPTH 0x0BA4
#define GL_TEXTURE_STACK_DEPTH 0x0BA5
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_TEXTURE_MATRIX 0x0BA8
#define GL_ALPHA_TEST_FUNC 0x0BC1
#define GL_ALPHA_TEST_REF 0x0BC2
#define GL_BLEND_DST 0x0BE0
#define GL_BLEND_SRC 0x0BE1
#define GL_SCISSOR_BOX 0x0C10
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_MAX_LIGHTS 0x0D31
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_MODELVIEW_STACK_DEPTH 0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH 0x0D38
#define GL_MAX_TEXTURE_STACK_DEPTH 0x0D39
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_RED_BITS 0x0D52
#define GL_GREEN_BITS 0x0D53
#define GL_BLUE_BITS 0x0D54
#define GL_ALPHA_BITS 0x0D55
#define GL_DEPTH_BITS 0x0D56
#define GL_STENCIL_BITS 0x0D57
#define GL_POLYGON_OFFSET_UNITS 0x2A00
#define GL_POLYGON_OFFSET_FACTOR 0x8038

/* GLU */
#define GLU_INVALID_ENUM 100900
#define GLU_INVALID_VALUE 100901
#define GLU_OUT_OF_MEMORY 100902

/* ---- context management (the "EGL" of inFlux) ---- */
typedef struct gl_context GLcontext;
GLcontext *glxCreateContext(int width, int height);      /* software framebuffer of width*height ARGB pixels */
void glxDestroyContext(GLcontext *ctx);
void glxMakeCurrent(GLcontext *ctx);
uint32_t *glxGetPixels(GLcontext *ctx);                  /* row-major, top row first, stride == width; alpha is the GL destination alpha */
void glxSetRendererName(const char *name);               /* reported through glGetString(GL_RENDERER) */
int glxGetWidth(GLcontext *ctx);
int glxGetHeight(GLcontext *ctx);

/* ---- state ---- */
void glClear(GLbitfield mask);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClearDepth(GLclampd d);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void glDepthRange(GLclampd n, GLclampd f);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glAlphaFunc(GLenum func, GLclampf ref);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glShadeModel(GLenum mode);
void glBlendFunc(GLenum src, GLenum dst);
void glPolygonMode(GLenum face, GLenum mode);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glPointSize(GLfloat size);
void glLineWidth(GLfloat width);
void glHint(GLenum target, GLenum mode);
void glFlush(void);
void glFinish(void);
GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);
void glGetBooleanv(GLenum pname, GLboolean *params);
void glGetIntegerv(GLenum pname, GLint *params);
void glGetFloatv(GLenum pname, GLfloat *params);
void glGetDoublev(GLenum pname, GLdouble *params);

/* ---- matrices ---- */
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glLoadMatrixd(const GLdouble *m);
void glMultMatrixf(const GLfloat *m);
void glMultMatrixd(const GLdouble *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glTranslated(GLdouble x, GLdouble y, GLdouble z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glScaled(GLdouble x, GLdouble y, GLdouble z);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);

/* ---- lighting and fog ---- */
void glLightf(GLenum light, GLenum pname, GLfloat param);
void glLighti(GLenum light, GLenum pname, GLint param);
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glGetLightfv(GLenum light, GLenum pname, GLfloat *params);
void glLightModelf(GLenum pname, GLfloat param);
void glLightModeli(GLenum pname, GLint param);
void glLightModelfv(GLenum pname, const GLfloat *params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params);
void glColorMaterial(GLenum face, GLenum mode);
void glFogf(GLenum pname, GLfloat param);
void glFogi(GLenum pname, GLint param);
void glFogfv(GLenum pname, const GLfloat *params);

/* ---- immediate mode ---- */
void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex2i(GLint x, GLint y);
void glVertex2d(GLdouble x, GLdouble y);
void glVertex2fv(const GLfloat *v);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3i(GLint x, GLint y, GLint z);
void glVertex3d(GLdouble x, GLdouble y, GLdouble z);
void glVertex3fv(const GLfloat *v);
void glVertex3dv(const GLdouble *v);
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertex4fv(const GLfloat *v);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor3d(GLdouble r, GLdouble g, GLdouble b);
void glColor3fv(const GLfloat *c);
void glColor3ub(GLubyte r, GLubyte g, GLubyte b);
void glColor3ubv(const GLubyte *c);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a);
void glColor4fv(const GLfloat *c);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void glColor4ubv(const GLubyte *c);
void glNormal3f(GLfloat x, GLfloat y, GLfloat z);
void glNormal3d(GLdouble x, GLdouble y, GLdouble z);
void glNormal3fv(const GLfloat *n);
void glTexCoord1f(GLfloat s);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2d(GLdouble s, GLdouble t);
void glTexCoord2fv(const GLfloat *v);
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r);
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2);
void glRecti(GLint x1, GLint y1, GLint x2, GLint y2);

/* ---- vertex arrays ---- */
void glEnableClientState(GLenum array);
void glDisableClientState(GLenum array);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glArrayElement(GLint i);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices);

/* ---- display lists ---- */
GLuint glGenLists(GLsizei range);
void glDeleteLists(GLuint list, GLsizei range);
GLboolean glIsList(GLuint list);
void glNewList(GLuint list, GLenum mode);
void glEndList(void);
void glCallList(GLuint list);
void glCallLists(GLsizei n, GLenum type, const GLvoid *lists);
void glListBase(GLuint base);

/* ---- textures and pixels ---- */
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
GLboolean glIsTexture(GLuint texture);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
void glPixelStorei(GLenum pname, GLint param);
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels);

/* ---- GLU ---- */
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble znear, GLdouble zfar);
void gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez, GLdouble cx, GLdouble cy, GLdouble cz, GLdouble ux, GLdouble uy, GLdouble uz);
void gluOrtho2D(GLdouble l, GLdouble r, GLdouble b, GLdouble t);
GLint gluProject(GLdouble ox, GLdouble oy, GLdouble oz, const GLdouble *model, const GLdouble *proj, const GLint *view, GLdouble *wx, GLdouble *wy, GLdouble *wz);
GLint gluUnProject(GLdouble wx, GLdouble wy, GLdouble wz, const GLdouble *model, const GLdouble *proj, const GLint *view, GLdouble *ox, GLdouble *oy, GLdouble *oz);
GLint gluBuild2DMipmaps(GLenum target, GLint internal, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data);

/* ---- small math helpers (also used by demos) ---- */
float glm_sinf(float x);
float glm_cosf(float x);
float glm_sqrtf(float x);
float glm_tanf(float x);
float glm_powf(float b, float e);
#define GLM_PI 3.14159265358979f
