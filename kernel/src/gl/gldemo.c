/* OpenGL Demo: a lit, mipmapped cube (vertex arrays) and a specular torus (display list) rendered through inFlux OpenGL. */
#include "ui.h"
#include "gpu.h"
#include "mm.h"
#include "string.h"
#include <GL/gl.h>

enum { DCMD_WIRE = CMD_APP + 1, DCMD_LIGHT, DCMD_TEXTURE, DCMD_SPIN, DCMD_CULL };
static const struct menu_item file_items[] = { MENU("Close", CMD_CLOSE, 'w', MOD_CMD), MENU("Quit OpenGL Demo", CMD_QUIT, 'q', MOD_CMD) };
static const struct menu_item view_items[] = { MENU("Wireframe", DCMD_WIRE, 'w', MOD_CMD | MOD_SHIFT), MENU("Lighting", DCMD_LIGHT, 'l', MOD_CMD), MENU("Texture", DCMD_TEXTURE, 't', MOD_CMD), MENU("Back-face Culling", DCMD_CULL, 'b', MOD_CMD), MENU_SEP, MENU("Animate", DCMD_SPIN, KEY_SPACE, 0) };
static const struct menu menus[] = { { "File", file_items, ARRAY_LEN(file_items) }, { "View", view_items, ARRAY_LEN(view_items) } };
static const struct app_menus demo_menus = { menus, ARRAY_LEN(menus) };

struct demo { struct window *w; GLcontext *gl; int fail_w, fail_h; char renderer[128]; GLuint tex, torus; float angle; bool wire, light, texture, spin, cull; int frames; uint64_t fps_t; int fps; int drag_x, drag_y; bool dragging; float rot_x, rot_y; };

static void make_texture(struct demo *d) {
    const int N = 64; GLubyte *px = kmalloc_try(N * N * 3);
    d->tex = 0;
    if (!px) return;   /* no texture: the cube is drawn untextured */
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int c = ((x / 8) + (y / 8)) & 1;
        GLubyte *p = px + (y * N + x) * 3;
        p[0] = c ? 240 : 40; p[1] = c ? 240 : 120; p[2] = c ? 240 : 220;
        if (((x & 7) == 0) || ((y & 7) == 0)) { p[0] = p[0] * 3 / 4; p[1] = p[1] * 3 / 4; p[2] = p[2] * 3 / 4; }
    }
    glGenTextures(1, &d->tex); glBindTexture(GL_TEXTURE_2D, d->tex);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, N, N, GL_RGB, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    kfree(px);
}

/* the cube as vertex arrays: 6 faces x 4 corners of position, normal, colour, texcoord */
static float cube_v[24][3], cube_n[24][3], cube_c[24][3], cube_t[24][2];
static void build_cube(void) {
    static const float n[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    static const float v[6][4][3] = {
        { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } }, { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },
        { { 1, -1, 1 }, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 } }, { { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } },
        { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } }, { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } } };
    static const float col[6][3] = { { 1, 0.3f, 0.3f }, { 0.3f, 1, 0.3f }, { 0.3f, 0.5f, 1 }, { 1, 0.9f, 0.3f }, { 1, 0.5f, 1 }, { 0.4f, 1, 1 } };
    static const float tc[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int f = 0; f < 6; f++) for (int k = 0; k < 4; k++) {
        int i = f * 4 + k;
        memcpy(cube_v[i], v[f][k], sizeof cube_v[i]); memcpy(cube_n[i], n[f], sizeof cube_n[i]);
        memcpy(cube_c[i], col[f], sizeof cube_c[i]); memcpy(cube_t[i], tc[k], sizeof cube_t[i]);
    }
}
static void draw_cube(void) {
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_NORMAL_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, cube_v); glNormalPointer(GL_FLOAT, 0, cube_n); glColorPointer(3, GL_FLOAT, 0, cube_c); glTexCoordPointer(2, GL_FLOAT, 0, cube_t);
    glDrawArrays(GL_QUADS, 0, 24);
    glDisableClientState(GL_VERTEX_ARRAY); glDisableClientState(GL_NORMAL_ARRAY); glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}
static void draw_torus(float R, float r, int nu, int nv) {
    for (int i = 0; i < nu; i++) {
        float u0 = (float)i / nu * 2 * GLM_PI, u1 = (float)(i + 1) / nu * 2 * GLM_PI;
        glBegin(GL_QUAD_STRIP);
        for (int j = 0; j <= nv; j++) {
            float vv = (float)j / nv * 2 * GLM_PI;
            float cv = glm_cosf(vv), sv = glm_sinf(vv);
            float us[2] = { u0, u1 };
            for (int k = 0; k < 2; k++) {
                float cu = glm_cosf(us[k]), su = glm_sinf(us[k]);
                glNormal3f(cv * cu, cv * su, sv);
                glColor3f(0.9f, 0.6f + 0.3f * cv, 0.2f);
                glVertex3f((R + r * cv) * cu, (R + r * cv) * su, r * sv);
            }
        }
        glEnd();
    }
}

static bool recreate(struct demo *d, int w, int h);

static void render(struct demo *d) {
    struct canvas *c = window_begin_draw(d->w);
    int cw = MIN(c->w, 8192), ch = MIN(c->h, 8192);   /* the largest GL framebuffer; a bigger window shows its top-left */
    mutex_unlock(&d->w->lock);
    if (!d->gl || ((cw != glxGetWidth(d->gl) || ch != glxGetHeight(d->gl)) && (cw != d->fail_w || ch != d->fail_h))) recreate(d, cw, ch);
    if (!d->gl || !glxMakeCurrent(d->gl)) {   /* no framebuffer (out of memory) or no current-context slot: say so */
        c = window_begin_draw(d->w);
        gfx_fill_rect(c, 0, 0, c->w, c->h, RGB(28, 28, 33));
        font_draw(c, font_ui_small, 16, 11, RGB(230, 230, 235), d->gl ? "OpenGL: no free current-context slot" : "OpenGL: not enough memory for a framebuffer of this size");
        window_end_draw(d->w, NULL);
        return;
    }
    int W = glxGetWidth(d->gl), H = glxGetHeight(d->gl);
    glViewport(0, 0, W, H);
    glClearColor(0.11f, 0.11f, 0.13f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluPerspective(45, (double)W / H, 0.5, 50);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); gluLookAt(0, 1.8, 7.5, 0, 0, 0, 0, 1, 0);
    glEnable(GL_DEPTH_TEST);
    if (d->cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, d->wire ? GL_LINE : GL_FILL);
    if (d->light) {
        glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_COLOR_MATERIAL);   /* glColor drives ambient and diffuse */
        float pos[4] = { 2, 4, 6, 0 }, dif[4] = { 1, 1, 1, 1 }, amb[4] = { 0.25f, 0.25f, 0.3f, 1 }, spec[4] = { 0.6f, 0.6f, 0.6f, 1 };
        glLightfv(GL_LIGHT0, GL_POSITION, pos); glLightfv(GL_LIGHT0, GL_DIFFUSE, dif); glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec); glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 32);
    }
    else glDisable(GL_LIGHTING);
    /* floor grid */
    glDisable(GL_TEXTURE_2D); glPushMatrix(); glTranslatef(0, -1.6f, 0); glColor3f(0.3f, 0.3f, 0.36f);
    glBegin(GL_LINES); for (int i = -6; i <= 6; i++) { glNormal3f(0, 1, 0); glVertex3f(i, 0, -6); glVertex3f(i, 0, 6); glVertex3f(-6, 0, i); glVertex3f(6, 0, i); } glEnd();
    glPopMatrix();
    /* cube */
    glPushMatrix(); glTranslatef(-1.6f, 0, 0); glRotatef(d->rot_x + d->angle * 0.7f, 1, 0, 0); glRotatef(d->rot_y + d->angle, 0, 1, 0);
    if (d->texture && !d->wire) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, d->tex); } else glDisable(GL_TEXTURE_2D);
    draw_cube(); glPopMatrix();
    glDisable(GL_TEXTURE_2D);
    /* torus */
    glPushMatrix(); glTranslatef(1.9f, 0.2f, 0); glRotatef(d->rot_x + d->angle * 1.3f, 1, 0.3f, 0); glRotatef(d->rot_y, 0, 1, 0);
    glCallList(d->torus); glPopMatrix();
    /* present into the window with a HUD */
    c = window_begin_draw(d->w);
    {
        int cw2 = MIN(W, c->w), ch2 = MIN(H, c->h);
        uint32_t *src = glxGetPixels(d->gl);
        if (cw2 < c->w || ch2 < c->h) gfx_fill_rect(c, 0, 0, c->w, c->h, RGB(28, 28, 33));   /* kept an older, smaller context */
        for (int y = 0; y < ch2; y++) memcpy(c->px + (size_t)y * c->stride, src + (size_t)y * W, (size_t)cw2 * 4);
    }
    char hud[160];
    snprintf(hud, sizeof hud, "%s  ·  %d fps  ·  %dx%d", glGetString(GL_RENDERER), d->fps, W, H);
    gfx_fill_rounded_rect(c, 8, 8, font_text_width(font_ui_small, hud) + 16, 20, 5, RGBA(0, 0, 0, 0x80));
    font_draw(c, font_ui_small, 16, 11, RGB(230, 230, 235), hud);
    const char *hint = "Drag to rotate · Space pauses · View menu toggles wireframe, lighting, texture";
    font_draw(c, font_ui_small, 16, c->h - 20, RGBA(255, 255, 255, 0x90), hint);
    window_end_draw(d->w, NULL);
    d->frames++;
    if (system_ticks - d->fps_t >= 1000) { d->fps = d->frames; d->frames = 0; d->fps_t = system_ticks; }
}

/* a new context at w x h. When it does not fit beside the old one, the old one is freed and w x h tried again, then the
 * old size (presented cropped or padded, and w x h is not retried every frame); with no context at all render() retries */
static bool recreate(struct demo *d, int w, int h) {
    int ow = glxGetWidth(d->gl), oh = glxGetHeight(d->gl);
    GLcontext *n = glxCreateContext(w, h);
    if (!n && d->gl) {
        glxDestroyContext(d->gl); d->gl = NULL;
        if (!(n = glxCreateContext(w, h))) { d->fail_w = w; d->fail_h = h; n = glxCreateContext(ow, oh); }
    }
    if (!n) { d->fail_w = w; d->fail_h = h; return false; }
    if (d->gl) glxDestroyContext(d->gl);
    d->gl = n;
    if (glxGetWidth(n) == w && glxGetHeight(n) == h) d->fail_w = d->fail_h = 0;
    glxSetRendererName(n, d->renderer);
    if (!glxMakeCurrent(n)) return true;   /* render() reports it */
    make_texture(d);
    /* the torus geometry never changes: compile it once per context */
    d->torus = glGenLists(1);
    glNewList(d->torus, GL_COMPILE); draw_torus(1.0f, 0.42f, 28, 16); glEndList();
    return true;
}

void gldemo_main(void *arg) {
    struct demo *d = kzalloc_try(sizeof *d);
    if (!d) return;
    d->light = d->texture = d->spin = d->cull = true;
    build_cube();
    struct gpu_device *g = gpu_primary();
    snprintf(d->renderer, sizeof d->renderer, "inFlux Software Rasterizer (%s)", g ? g->name : "framebuffer");   /* per context: other demos may be rendering */
    d->w = window_create("OpenGL Demo", 560, 400, WIN_STANDARD);
    window_set_min_size(d->w, 240, 180);
    window_set_menus(d->w, &demo_menus);
    recreate(d, 560, 400);
    d->fps_t = system_ticks;
    render(d);
    window_show(d->w);
    window_set_timer(d->w, 16);
    struct event ev; bool quit = false;
    while (!quit && window_wait_event(d->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: case EV_CLOSE: quit = true; break;
        case EV_MENU:
            if (ev.cmd == CMD_QUIT || ev.cmd == CMD_CLOSE) quit = true;
            else if (ev.cmd == DCMD_WIRE) d->wire = !d->wire; else if (ev.cmd == DCMD_LIGHT) d->light = !d->light;
            else if (ev.cmd == DCMD_TEXTURE) d->texture = !d->texture; else if (ev.cmd == DCMD_SPIN) d->spin = !d->spin; else if (ev.cmd == DCMD_CULL) d->cull = !d->cull;
            render(d); break;
        case EV_TIMER: if (d->spin) { d->angle += 1.2f; if (d->angle > 360) d->angle -= 360; render(d); } break;
        case EV_RESIZE: render(d); break;   /* render() recreates the context at the new canvas size */
        case EV_KEY_DOWN: if (ev.key == KEY_SPACE) d->spin = !d->spin; else if (ev.ch == 'w') d->wire = !d->wire; else if (ev.ch == 'l') d->light = !d->light; else if (ev.ch == 't') d->texture = !d->texture; render(d); break;
        case EV_MOUSE_DOWN: d->dragging = true; d->drag_x = ev.x; d->drag_y = ev.y; break;
        case EV_MOUSE_UP: d->dragging = false; break;
        case EV_MOUSE_MOVE: if (d->dragging) { d->rot_y += (ev.x - d->drag_x) * 0.6f; d->rot_x += (ev.y - d->drag_y) * 0.6f; d->drag_x = ev.x; d->drag_y = ev.y; if (!d->spin) render(d); } break;
        default: break;
        }
    }
    window_destroy(d->w);
    glxDestroyContext(d->gl);   /* also drops it as this thread's current context */
    kfree(d);
}
