#pragma once
/* HTML parsing and layout for the web browser (kernel/src/apps/html.c, layout.c). */
#include "gfx.h"

enum html_tag {
    T_UNKNOWN = 0, T_HTML, T_HEAD, T_BODY, T_TITLE, T_META, T_LINK, T_SCRIPT, T_STYLE, T_BASE,
    T_P, T_DIV, T_SPAN, T_A, T_B, T_STRONG, T_I, T_EM, T_U, T_S, T_STRIKE, T_DEL, T_INS,
    T_CODE, T_TT, T_KBD, T_SAMP, T_VAR, T_PRE, T_XMP, T_LISTING, T_PLAINTEXT, T_BR, T_HR,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_UL, T_OL, T_LI, T_DL, T_DT, T_DD, T_MENU, T_DIR,
    T_BLOCKQUOTE, T_CENTER, T_TABLE, T_CAPTION, T_THEAD, T_TBODY, T_TFOOT, T_TR, T_TD, T_TH,
    T_COL, T_COLGROUP, T_IMG, T_FORM, T_INPUT, T_BUTTON, T_SELECT, T_OPTION, T_OPTGROUP,
    T_TEXTAREA, T_LABEL, T_FIELDSET, T_LEGEND, T_FONT, T_SMALL, T_BIG, T_SUB, T_SUP,
    T_HEADER, T_FOOTER, T_NAV, T_SECTION, T_ARTICLE, T_ASIDE, T_MAIN, T_ADDRESS, T_FIGURE,
    T_FIGCAPTION, T_NOSCRIPT, T_IFRAME, T_SVG, T_MATH, T_NOBR, T_WBR, T_ABBR, T_ACRONYM,
    T_CITE, T_Q, T_MARK, T_DFN, T_DETAILS, T_SUMMARY, T_AREA, T_MAP, T_VIDEO, T_AUDIO,
    T_SOURCE, T_TRACK, T_CANVAS, T_OBJECT, T_EMBED, T_PARAM, T_TEMPLATE, T_HGROUP, T_DIALOG,
    T_PICTURE, T_TIME, T_DATA, T_OUTPUT, T_METER, T_PROGRESS, T_BLINK, T_MARQUEE, T_FRAMESET,
    T_FRAME, T_NOFRAMES,
    T_COUNT
};

enum { HN_ELEMENT, HN_TEXT };

struct html_attr { char *name, *value; };

struct form_field;
struct html_image;

struct html_node {
    int type;
    int tag;                       /* enum html_tag */
    char *text;                    /* HN_TEXT: decoded UTF-8 */
    int text_len;
    struct html_attr *attrs; int nattrs;
    struct html_node *parent, *first, *last, *next;
    struct form_field *field;      /* form controls */
    struct html_image *image;      /* <img> */
};

struct html_image {
    char *url;                     /* absolute */
    struct canvas *img;            /* decoded, NULL until loaded */
    struct canvas *scaled;         /* cached copy at the size it was last painted */
    bool failed;
    struct html_image *next;
    struct html_image *hnext;      /* parser's URL hash chain */
};

enum { FF_TEXT, FF_PASSWORD, FF_SUBMIT, FF_RESET, FF_BUTTON, FF_CHECKBOX, FF_RADIO, FF_HIDDEN, FF_SELECT, FF_TEXTAREA, FF_IMAGE };
struct form_field {
    struct html_node *node, *form;
    int kind;
    char name[128];
    char value[512];
    bool checked;
    int selected;                  /* FF_SELECT: option index */
    int noptions;
    struct form_field *next;
};

struct html_doc {
    struct html_node *root;
    char title[256];
    char url[1024];                /* document URL (for resolving links) */
    char base[1024];               /* <base href> or url */
    color_t bg, text, link, vlink;
    bool has_bg;
    struct html_image *images;
    struct form_field *fields;
    char *source; size_t source_len;   /* the raw document, for View Source */
};

/* parse a document; data is raw bytes, charset a hint from Content-Type (may be NULL) */
struct html_doc *html_parse(const char *data, size_t len, const char *charset, const char *url);
/* a document holding plain text in a <pre> */
struct html_doc *html_from_text(const char *data, size_t len, const char *url);
void html_free(struct html_doc *d);
const char *html_attr(const struct html_node *n, const char *name);
bool html_parse_color(const char *s, color_t *out);
/* bytes -> UTF-8 (decodes windows-1252 when the input is not valid UTF-8 or the hint says latin-1) */
char *html_to_utf8(const char *data, size_t len, const char *charset, size_t *out_len);

/* ---- layout ---- */
enum { DI_TEXT, DI_RECT, DI_BORDER, DI_IMAGE, DI_HLINE, DI_FIELD, DI_BULLET };
#define DIF_UNDERLINE 1
#define DIF_STRIKE    2
#define DIF_ITALIC    4
#define DIF_PLACEHOLDER 8

struct display_item {
    int type;
    int x, y, w, h;                /* document coordinates */
    color_t color;
    struct font *font;
    int flags;
    const char *text; int len;
    int link;                      /* index into layout->links, -1 none */
    struct html_node *node;        /* image / field source */
    int ul_w;                      /* underline width (reaches into the following space inside a link) */
};

struct layout_anchor { char *name; int y; };

struct layout {
    struct display_item *items; int nitems, cap;
    char **links; int nlinks;
    struct html_node **link_nodes;
    struct layout_anchor *anchors; int nanchors;
    char **strs; int nstrs;        /* generated text referenced by items */
    int width, height;             /* document extent */
    color_t bg, link_color, vlink_color;
};

struct layout *html_layout(struct html_doc *doc, int width);
void layout_free(struct layout *l);
int layout_link_at(struct layout *l, int x, int y);
struct display_item *layout_field_at(struct layout *l, int x, int y);
int layout_anchor_y(struct layout *l, const char *name);   /* -1 if missing */
void layout_paint(struct layout *l, struct canvas *c, int scroll_y, struct form_field *focus, bool (*visited)(void *ctx, const char *url), void *ctx);
/* text and value of the index-th <option> of a <select> */
const char *select_option_text(struct html_node *sel, int index, char *out, size_t outsz, const char **value);
