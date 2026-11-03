/* glcube: a user-mode OpenGL program. Renders with inFlux GL and presents into a desktop window. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <influx.h>
#include <GL/gl.h>

static int W = 480, H = 360;

static void cube(void) {
    static const float n[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    static const float v[6][4][3] = {
        { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } }, { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },
        { { 1, -1, 1 }, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 } }, { { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } },
        { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } }, { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } } };
    static const float col[6][3] = { { 0.9f, 0.2f, 0.2f }, { 0.2f, 0.8f, 0.3f }, { 0.2f, 0.4f, 1 }, { 1, 0.8f, 0.2f }, { 0.9f, 0.3f, 0.9f }, { 0.3f, 0.9f, 0.9f } };
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) { glNormal3fv(n[f]); glColor3fv(col[f]); for (int k = 0; k < 4; k++) glVertex3fv(v[f][k]); }
    glEnd();
}

int main(int argc, char **argv) {
    int win = win_create(W, H, "glcube (user mode)");
    if (win < 0) { fprintf(2, "glcube: cannot create a window\n"); return 1; }
    GLcontext *gl = glxCreateContext(W, H);
    if (!gl) { fprintf(2, "glcube: cannot create a GL context\n"); return 1; }
    glxMakeCurrent(gl);
    printf("glcube: %s %s on %s\n", glGetString(GL_VENDOR), glGetString(GL_VERSION), glGetString(GL_RENDERER));
    float angle = 0; int frames = 0; long t0 = uptime_ms();
    for (;;) {
        struct win_event ev;
        while (win_event(win, &ev, 0) == 1) {
            if (ev.type == WEV_RESIZE && ev.w > 0 && ev.h > 0 && (ev.w != W || ev.h != H)) {
                GLcontext *n = glxCreateContext(ev.w, ev.h);
                if (n) { glxDestroyContext(gl); gl = n; W = ev.w; H = ev.h; glxMakeCurrent(gl); }
            }
            if (ev.type == WEV_CLOSE || ev.type == WEV_QUIT || (ev.type == WEV_MENU && ev.cmd == WCMD_QUIT) || (ev.type == WEV_KEY_DOWN && ev.key == WKEY_ESC)) goto done;
        }
        glViewport(0, 0, W, H);
        glClearColor(0.08f, 0.09f, 0.12f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE); glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_COLOR_MATERIAL);
        float spec[4] = { 0.5f, 0.5f, 0.5f, 1 }; glMaterialfv(GL_FRONT, GL_SPECULAR, spec); glMaterialf(GL_FRONT, GL_SHININESS, 24);
        float lp[4] = { 1, 2, 3, 0 }; glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glLightfv(GL_LIGHT0, GL_POSITION, lp);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluPerspective(50, (double)W / H, 0.5, 20);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(0, 0, -5.5f);
        glRotatef(angle, 1, 0, 0); glRotatef(angle * 1.6f, 0, 1, 0);
        cube();
        win_present(win, glxGetPixels(gl));
        angle += 1.5f; frames++;
        if (uptime_ms() - t0 >= 5000) { char t[64]; snprintf(t, sizeof t, "glcube (user mode) — %ld fps", frames * 1000 / (uptime_ms() - t0)); win_set_title(win, t); frames = 0; t0 = uptime_ms(); }
        sleep_ms(16);
    }
done:
    glxDestroyContext(gl);
    win_destroy(win);
    return 0;
}
