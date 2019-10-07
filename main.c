/*
 * main.c
 *
 * Implementation of mangl graphical man pages viewer
 *
 * Initial commit: 2019-09-07
 *
 * Author: Ziga Lenarcic <ziga.lenarcic@gmail.com>
 */

#include <sys/types.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <glob.h>
#include <err.h>
#include <stdbool.h>
#include <GL/gl.h>
#ifdef __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#include "stretchy_buffer.h"
#include "hashmap.h"

#include "mandoc/mandoc.h"
#include "mandoc/roff.h"
#include "mandoc/mandoc_parse.h"
#include "mandoc/manconf.h"
#include "mandoc/out.h"
#include "mandoc/mandoc_aux.h"
#include "mandoc/term.h"

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
#define ZMALLOC(type, n) ((type *)calloc(n, sizeof(type)))

#define FONT_TEXTURE_SIZE 256
const static unsigned int font_image_width = 112;
const static unsigned int font_image_height = 84;

#include "font_image.h"

enum DISPLAY_MODES {
    D_MANPAGE = 0,
    D_SEARCH = 1
};

int display_mode = D_MANPAGE;
char search_term[512];

char **manpage_names;

#define N_SHOWN_RESULTS 12
int results_selected_index = 0;
int results_shown_lines = N_SHOWN_RESULTS;
int results_view_offset = 0;
const static int search_width = 300;

struct {
    int idx;
    int goodness;
} matches[100];
int matches_count = 0;

const unsigned int font_char_width = 7;
const unsigned int font_char_height = 14;

const static int scrollbar_width = 12;
const static int scrollbar_thumb_margin = 0;

int scrollbar_thumb_position;
int scrollbar_thumb_size;
int scrollbar_thumb_hover;

int scrollbar_dragging = 0;
int scrollbar_thumb_mouse_down_y = 0;
int scrollbar_thumb_mouse_down_thumb_position = 0;
const static int document_margin = 29;

int window_width;
int window_height;

GLuint font_texture;

float doc_scale = 0.5f;
int scroll_amount = 40;

map_t manpage_database;

void terminal_mdoc(void *, const struct roff_meta *);
void terminal_man(void *, const struct roff_meta *);

typedef struct {
    int x;
    int y;
    int x2;
    int y2;
} recti;

typedef struct {
    recti document_rectangle;
    int highlight;
    char link[256];
} link_t;

bool inside_recti(recti r, int x, int y)
{
    return (x >= r.x) && (y >= r.y) && (x < r.x2) && (y < r.y2);
}

double clamp(double val, double min, double max)
{
    if (val > max) return max;
    if (val < min) return min;
    return val;
}

void print_usage(const char *exe)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "%s\n", exe);
    fprintf(stderr, "%s [section] man_page\n", exe);
    fprintf(stderr, "%s -f man_page_file\n", exe);
    fprintf(stderr, "\n");

    exit(1);
}

enum SPAN_TYPE
{
    SPAN_TITLE = 1,
    SPAN_TEXT = 2,
    SPAN_SECTION = 3,
    SPAN_LINK = 4,
    SPAN_URL = 5,
};

struct span {
    char *buffer;
    int buffer_size;
    int length;
    int type;

    struct span *next;
};

struct manpage
{
    char manpage_name[128];
    char manpage_section[64];
    char filename[512];

    struct {
        struct span **lines;
        int n_lines;
        int lines_allocated;
    } document;

    int scroll_position;

    link_t *links;
};

struct manpage *page;
struct manpage *formatting_page; // used when formatting page

struct manpage **page_stack;
size_t stack_pos; // index of displayed page + 1

void open_new_page(const char *filename);
void page_back(void);
void page_forward(void);

void add_line(struct manpage *p)
{
#define STARTING_LINES 256
    if (p->document.n_lines == 0)
    {
        p->document.lines = ZMALLOC(struct span *, STARTING_LINES);
        p->document.lines_allocated = STARTING_LINES;
    }
    else if (p->document.n_lines >= p->document.lines_allocated)
    {
        int new_n_lines = p->document.lines_allocated * 2;
        struct span **old_lines = p->document.lines;
        p->document.lines = ZMALLOC(struct span *, new_n_lines);
        memcpy(p->document.lines, old_lines, sizeof(struct span *) * p->document.n_lines);
        free(old_lines);
        p->document.lines_allocated = new_n_lines;
    }

    p->document.n_lines++;
    p->document.lines[p->document.n_lines - 1] = ZMALLOC(struct span, 1);
}

struct span* get_last_span(struct manpage *p)
{
    if (p->document.n_lines == 0) return NULL;

    struct span *s = p->document.lines[p->document.n_lines - 1];
    while (s && s->next)
    {
        s = s->next;
    }

    return s;
}

void add_to_span(struct span *s, int letter)
{
#define STARTING_SPAN_SIZE 32
    char letter_2 = 0;

    if ((letter >= 0x2500) && (letter <= 0x2501))
        letter = '-';
    else if ((letter >= 0x2502) && (letter <= 0x2503))
        letter = '|';
    else if ((letter >= 0x250c) && (letter <= 0x254b)) /* various cross symbols */
        letter = '+';
    else if (letter == 0x2014)
        letter = '-';
    else if (letter == 0x2212)
        letter = '-';
    else if (letter == 0x2002)
        letter = ' ';
    else if (letter == 0x2010)
        letter = '-';
    else if (letter == 0x2013) /* en dash */
        letter = '-';
    else if (letter == 0x2022) /* bullet */
        letter = '-';
    else if (letter == 0x2265) /* greater than or equel */
    {
        letter = '>';
        letter_2 = '=';
    }
    else if (letter == 0x2264) /* less than or equel */
    {
        letter = '<';
        letter_2 = '=';
    }
    else if (letter == 160) /* non breaking space */
        letter = ' ';
    else if ((letter == 0x201c) || (letter == 0x201d)) /* left and right double quotation mark */
        letter = '"';

    if (letter < 256)
    {
        int chars = (letter_2 > 0) ? 2 : 1;
        if (s->buffer_size == 0)
        {
            s->buffer = ZMALLOC(char, STARTING_SPAN_SIZE);
            s->buffer_size = STARTING_SPAN_SIZE;
            s->length = 0;
        }
        else if ((s->length + chars) >= s->buffer_size)
        {
            int new_buffer_size = s->buffer_size * 2;

            char *old_buffer = s->buffer;
            s->buffer = ZMALLOC(char, new_buffer_size);
            memcpy(s->buffer, old_buffer, s->length);
            free(old_buffer);
            s->buffer_size = new_buffer_size;
        }

        s->buffer[s->length++] = letter;
        if (letter_2 > 0)
            s->buffer[s->length++] = letter_2;

    }
    else
    {
        fprintf(stderr, "Letter %d, 0x%x\n", letter, letter);
    }
}

void free_span(struct span *s)
{
    while (s)
    {
        if (s->buffer)
            free(s->buffer);

        struct span *tmp = s;
        s = s->next;
        free(tmp);
    }
}

void free_manpage(struct manpage *p)
{
    for (int i = 0; i < p->document.n_lines; i++)
        free_span(p->document.lines[i]);
}

static void format_headf(struct termp *p, const struct roff_meta *meta)
{
    //printf("%s\n", __func__);
}

static void format_footf(struct termp *p, const struct roff_meta *meta)
{
    //printf("%s\n", __func__);
}

static void format_letter(struct termp *p, int letter)
{
    struct span *s = get_last_span(formatting_page);
    add_to_span(s, letter);
}

static void format_begin(struct termp *p)
{
    //printf("%s\n", __func__);
    (*p->headf)(p, p->argf);
}

static void format_end(struct termp *p)
{
    //printf("%s\n", __func__);
    (*p->footf)(p, p->argf);
}

static void format_endline(struct termp *p)
{
    //printf("%s\n", __func__);
    p->line++;
    p->tcol->offset -= p->ti;
    p->ti = 0;

    add_line(formatting_page);
}

static void format_advance(struct termp *p, size_t len)
{
    //printf("%s %zu\n", __func__, len);

    for (int i = 0; i < len; i++)
    {
        //printf(" ");

        struct span *s = get_last_span(formatting_page);
        add_to_span(s, ' ');
    }
}

static void format_setwidth(struct termp *p, int a, int b)
{
    //printf("%s\n", __func__);
}

static size_t format_width(const struct termp *p, int a)
{
    return a != ASCII_BREAK; /* 1 unless it's ASCII_BREAK (zero width space) */
}

static int format_hspan(const struct termp *p, const struct roffsu *su)
{
    //printf("%s\n", __func__);

    double r = 0.0;

    switch (su->unit)
    {
        case SCALE_BU:
            r = su->scale;
            break;
        case SCALE_CM:
            r = su->scale * 240.0 / 2.54;
            break;
        case SCALE_FS:
            r = su->scale * 65536.0;
            break;
        case SCALE_IN:
            r = su->scale * 240.0;
            break;
        case SCALE_MM:
            r = su->scale * 0.24;
            break;
        case SCALE_VS:
        case SCALE_PC:
            r = su->scale * 40.0;
            break;
        case SCALE_PT:
            r = su->scale * 10.0 / 3.0;
            break;
        case SCALE_EN:
        case SCALE_EM:
            r = su->scale * 24.0;
            break;
        default:
            fprintf(stderr, "Unknown unit.\n");
            break;
    }

    return (r > 0.0) ? (r + 0.01) : (r - 0.01);
}

void *mangl_formatter(int width, int indent)
{
    struct termp *p = mandoc_calloc(1, sizeof(struct termp));

    p->tcol = p->tcols = mandoc_calloc(1, sizeof(struct termp_col));
    p->maxtcol = 1;

    p->line = 1;
    p->defrmargin = p->lastrmargin = width;

    /* allocate font stack */
    p->fontsz = 8;
    p->fontq = mandoc_reallocarray(NULL, p->fontsz, sizeof(*p->fontq));
    p->fontq[0] = p->fontl = TERMFONT_NONE;

    p->synopsisonly = 0;

    // enable if mdoc style is needed
    p->mdocstyle = 1;
    p->defindent = indent; // change indent

    p->flags = 0;

    p->type = TERMTYPE_CHAR;
    p->enc = TERMENC_UTF8;

    /* functions */
    p->headf = &format_headf;
    p->footf = &format_footf;
    p->letter = &format_letter;
    p->begin = &format_begin;
    p->end = &format_end;
    p->endline = &format_endline;
    p->advance = &format_advance;
    p->setwidth = &format_setwidth;
    p->width = &format_width;
    p->hspan = &format_hspan;

    p->ps = NULL;

    return p;
}

void mangl_formatter_free(void *formatter)
{
    struct termp *p = (struct termp *)formatter;
    term_free(p);
}

void display_manpage_stdout(struct manpage *p)
{
    printf("Manpage to stdout:\n");
    for (int i = 0; i < p->document.n_lines; i++)
    {
        struct span *s = p->document.lines[i];

        while (s)
        {
            if (s->length > 0)
                printf("%s", s->buffer);
            s = s->next;
        }

        //printf(" .END OF LINE\n");
        printf("\n");
    }
    printf(".END OF MANPAGE\n");
}

int line_height(float scale)
{
    return font_char_height;
}

int character_width(float scale)
{
    return font_char_width;
}

int document_width(void)
{
    return 2 * document_margin + ((78 + 2) * character_width(doc_scale));
}

int document_height(void)
{
    return page->document.n_lines * line_height(doc_scale) + 2 * document_margin;
}

void find_links(struct manpage *p)
{
    int vertical_position = 0;

    for (int i = 0; i < p->document.n_lines; i++)
    {
        struct span *s = p->document.lines[i];

        char line[2048];
        line[0] = 0;

        while (s)
        {
            if (s->length > 0)
            {
                //draw_string(s->buffer, document_margin, document_margin + vertical_position - page->scroll_position, doc_scale);

                int pos = 0;

                char *in = s->buffer;

                while (*in && (pos < (ARRAY_SIZE(line) - 1)))
                {
                    if (*in == '\b')
                    {
                        if (pos > 0) pos--;
                    }
                    else
                    {
                        line[pos++] = *in;
                    }

                    in++;
                }

                line[pos] = 0;

                // search links

                char current_word[256];
                int word_pos = 0;
                int opening_paren = 0;

                /* custom parser */
                char *str = line;

                while (*str)
                {
                    if ((*str == ' ') || (*str == ',') || (*str == '\t') || (*str == '\n') || (*str == '\r'))
                    {
                        word_pos = 0;
                        opening_paren = 0;
                        str++;
                        continue;
                    }

                    /* can't start the word with parenthesis */
                    if ((word_pos == 0) && ((*str == '(') || (*str == ')') || (*str == '|')))
                    {
                        opening_paren = 0;
                        str++;
                        continue;
                    }

                    current_word[word_pos++] = *str;

                    if (*str == '(')
                        opening_paren = 1;
                    else if (*str == ')')
                    {
                        if (opening_paren)
                        {
                            /* word is complete */
                            current_word[word_pos] = 0;
                            char *tmp;
                            if (hashmap_get(manpage_database, current_word, strlen(current_word), (void **)&tmp) == MAP_OK)
                            {
                                /* we have a link */
                                link_t l;
                                l.document_rectangle.x = ((intptr_t)str - (intptr_t)line + 1 - strlen(current_word)) * character_width(doc_scale);
                                l.document_rectangle.y = i * line_height(doc_scale);
                                l.document_rectangle.x2 = l.document_rectangle.x + strlen(current_word) * character_width(doc_scale);
                                l.document_rectangle.y2 = l.document_rectangle.y + line_height(doc_scale);

                                strcpy(l.link, tmp);
                                l.highlight = 0;

                                sb_push(p->links, l);
                            }

                            word_pos = 0;
                            opening_paren = 0;
                            str++;
                            continue;
                        }
                    }

                    str++;
                }

            }
            s = s->next;
        }

        vertical_position += line_height(doc_scale);

    }

}

void update_scrollbar(void)
{
    if (display_mode == D_SEARCH)
        return;

    int doc_height = document_height();
    int thumb_size_tmp = (double)window_height / (doc_height - 1) * window_height;

    scrollbar_thumb_size = clamp(thumb_size_tmp, 20, window_height);
    //scrollbar_thumb_position = round((double)page->scroll_position / (doc_height - 1) * window_height);
    scrollbar_thumb_position = round((double)page->scroll_position / (doc_height - window_height) * (window_height - scrollbar_thumb_size));
}

int scrollbar_thumb_position_to_scroll_position(int thumb_position)
{
    int doc_height = document_height();
    int thumb_size_tmp = (double)window_height / (doc_height - 1) * window_height;

    scrollbar_thumb_size = clamp(thumb_size_tmp, 20, window_height);

    int scrollbar_height = window_height;

    double percentage = (double)thumb_position / (scrollbar_height - scrollbar_thumb_size);

    return percentage * (doc_height - window_height);
}

void reshape(int w, int h)
{
    //printf("Reshape %d x %d\n", w, h);

    window_width = w;
    window_height = h;

    glViewport(0, 0, window_width, window_height);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity(); /* must reset, further calls modify the matrix */

    glOrtho(0 /*left*/,
            window_width /*right*/,
            window_height /*bottom*/,
            0 /*top*/,
            -1 /*nearVal*/,
            1 /*farVal*/);

    glMatrixMode(GL_MODELVIEW);

    update_scrollbar();
}

int fitting_window_width(float scale)
{
    return 2 * document_margin + ((78 + 2) * character_width(scale)) + scrollbar_width;
}

float color_table[][3] = {
    {21.0f/255.0f, 21.0f/255.0f, 21.0f/255.0f},
    {232.0f/255.0f, 232.0f/255.0f, 211.0f/255.0f},
    {143.0f/255.0f, 191.0f/255.0f, 220.0f/255.0f},
    {255.0f/255.0f, 185.0f/255.0f, 100.0f/255.0f},
    {0.4, 0.4, 0.4},
    {0.15, 0.15, 0.15},
    {0.27, 0.27, 0.27},
    {0.33, 0.33, 0.33},
    {0.2, 0.2, 0.2},
    {235.0f/255.0f, 180.0f/255.0f, 112.0f/255.0f},
};

enum {
    COLOR_INDEX_BACKGROUND = 0,
    COLOR_INDEX_FOREGROUND,
    COLOR_INDEX_BOLD,
    COLOR_INDEX_ITALIC,
    COLOR_INDEX_DIM,
    COLOR_INDEX_SCROLLBAR_BACKGROUND,
    COLOR_INDEX_SCROLLBAR_THUMB,
    COLOR_INDEX_SCROLLBAR_THUMB_HOVER,
    COLOR_INDEX_LINK_BACKGROUND,
    COLOR_INDEX_PAGE_BORDER,
};

void set_color(int i)
{
    glColor3f(color_table[i][0], color_table[i][1], color_table[i][2]);
}

void draw_rectangle(int x, int y, int w, int h)
{
    glBegin(GL_TRIANGLE_STRIP);
    glVertex2i(x, y);
    glVertex2i(x + w, y);
    glVertex2i(x, y + h);
    glVertex2i(x + w, y + h);
    glEnd();
}

void draw_rectangle_outline(int x, int y, int w, int h)
{
    glTranslatef(-0.5, -0.5, 0); /* fix missing pixel in the corner */
    glBegin(GL_LINE_STRIP);
    glVertex2i(x, y);
    glVertex2i(x + w, y);
    glVertex2i(x + w, y + h);
    glVertex2i(x, y + h);
    glVertex2i(x, y);
    glEnd();
    glTranslatef(0.5, 0.5, 0);
}

/*
   !"#$%&'()*+,-./
0123456789:;<=>?
@ABCDEFGHIJKLMNO
PQRSTUVWXYZ[\]^_
`abcdefghijklmno
pqrstuvwxyz{|}~
*/

void put_char_gl(int x, int y, char c)
{
    static const float pixel = 1.0f / FONT_TEXTURE_SIZE;
    int w = font_char_width;
    int h = font_char_height;

    glBindTexture(GL_TEXTURE_2D, font_texture);
    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);

    if (c < 32)
    {
        // unknown character
        glDisable(GL_BLEND);
        draw_rectangle_outline(x + 1, y + 1, w - 2, h - 2);
        glEnable(GL_BLEND);
    }
    else
    {
        c -= 32;

        char c_line = c / 16;
        char c_pos = c % 16;

        glBegin(GL_QUADS);
        glTexCoord2f(c_pos * pixel * w, c_line * h * pixel); glVertex2f(x, y);
        glTexCoord2f(c_pos * pixel * w, (c_line + 1) * h * pixel); glVertex2f(x, y + h);
        glTexCoord2f((c_pos + 1) * pixel * w, (c_line + 1) * h * pixel); glVertex2f(x + w, y + h);
        glTexCoord2f((c_pos + 1) * pixel * w, c_line * h * pixel); glVertex2f(x + w, y);
        glEnd();
    }

    glDisable(GL_BLEND);
}

void print_text_gl(int x, int y, const char *str)
{
    while (*str)
    {
        put_char_gl(x, y, *str);
        x += font_char_width;
        str++;
    }
}

size_t draw_string_manpage(const char *str, int x, int y, float scale)
{
    set_color(COLOR_INDEX_FOREGROUND);
    size_t count = 0;
    while (*str)
    {
        int color_set = 0;
        if (str[1] == '\b') /* next character is backspace */
        {
            if (str[2] == str[0])
            {
                set_color(COLOR_INDEX_BOLD);
            }
            else if (str[0] == '_')
            {
                set_color(COLOR_INDEX_ITALIC);
            }
            else
            {
                set_color(COLOR_INDEX_DIM);
            }

            str += 2;
            color_set = 1;
            if (*str == '\0')
                break;
        }

        put_char_gl(x + count++ * character_width(doc_scale), y, *str);

        if (color_set)
        {
            set_color(COLOR_INDEX_FOREGROUND);
        }
        str++;
    }

    return count;
}

size_t draw_string(const char *str, int x, int y, float scale)
{
    size_t count = 0;
    while (*str)
    {
        put_char_gl(x + count++ * character_width(doc_scale), y, *str);
        str++;
    }

    return count;
}

void init_gl(void)
{
    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);

    unsigned char texture[FONT_TEXTURE_SIZE * FONT_TEXTURE_SIZE];
    memset(texture, 0, sizeof(texture));

    char *font_data = font_image;

    for (int j = 0; j < font_image_height; j++)
    {
        for (int i = 0; i < font_image_width; i++)
        {
            texture[j * 256 + i] = (*font_data++) ? 255 : 0;
        }
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, FONT_TEXTURE_SIZE, FONT_TEXTURE_SIZE, 0, GL_ALPHA, GL_UNSIGNED_BYTE, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

void render_manpage(struct manpage *p)
{
    int vertical_position = 0;

    for (int i = 0; i < p->document.n_lines; i++)
    {
        struct span *s = p->document.lines[i];

        if ((vertical_position >= (page->scroll_position - line_height(doc_scale) - document_margin)) &&
                ((vertical_position - line_height(doc_scale)) < (page->scroll_position + window_height)))
        {
            int num_chars = 0;
            while (s)
            {
                if (s->length > 0)
                {
                    num_chars += draw_string_manpage(s->buffer,
                            document_margin + num_chars * character_width(doc_scale),
                            document_margin + vertical_position - page->scroll_position, doc_scale);
                }
                s = s->next;
            }
        }

        vertical_position += line_height(doc_scale);

        if ((vertical_position - line_height(doc_scale)) > (page->scroll_position + window_height))
            break;
    }
}

int find_string(const char *search_term, const char *text)
{
    int search_len = strlen(search_term);
    int text_len = strlen(text);

    for (int i = 0; i < (text_len - search_len); i++)
    {
        bool match = true;
        for (int j = 0; j < search_len; j++)
        {
            if (search_term[j] != text[i + j])
            {
                match = false;
                break;
            }
        }

        if (match)
            return i;
    }

    return -1;
}

int compar_match(const void *a, const void *b)
{
    return ((const int *)b)[1] - ((const int *)a)[1];
}

void update_search(void)
{
    int search_term_len = strlen(search_term);

    memset(matches, 0, sizeof(matches));
    matches_count = 0;

    results_view_offset = 0;
    results_selected_index = 0;

    if (search_term_len == 0)
    {
        return;
    }
    else
    {
        int count = sb_count(manpage_names);

        for (int i = 0; i < count; i++)
        {
            int position = find_string(search_term, manpage_names[i]);

            if (position >= 0)
            {
                matches[matches_count].idx = i;
                matches[matches_count].goodness = -position * 100 - (strlen(manpage_names[i]) - search_term_len);
                matches_count++;

                if (matches_count >= ARRAY_SIZE(matches))
                {
                    break;
                }
            }
        }

        qsort(matches, matches_count, sizeof(matches[0]),
                &compar_match);
    }
}

void render(void)
{
    glClearColor(color_table[COLOR_INDEX_BACKGROUND][0], color_table[COLOR_INDEX_BACKGROUND][1], color_table[COLOR_INDEX_BACKGROUND][2], 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    switch (display_mode)
    {
        case D_MANPAGE:
            {
                render_manpage(page);

                /* draw document border */
                int border_margin = document_margin * 3 / 8 + 1;
                set_color(COLOR_INDEX_PAGE_BORDER);
                draw_rectangle_outline(border_margin, border_margin - page->scroll_position,
                        document_width() - 2 * border_margin, document_height() - 2 * border_margin);

                /* draw link hovering */
                {
                    int link_number = sb_count(page->links);
                    set_color(COLOR_INDEX_FOREGROUND);
                    for (int i = 0; i < link_number; i++)
                    {
                        recti r = page->links[i].document_rectangle;

                        r.x += document_margin;
                        r.x2 += document_margin;
                        r.y += document_margin - page->scroll_position;
                        r.y2 += document_margin - page->scroll_position;

                        if ((r.y2 >= 0) || (r.y < window_height))
                        {
                            if (page->links[i].highlight)
                            {
                                //draw_rectangle_outline(r.x, r.y, r.x2 - r.x, r.y2 - r.y);
                                glEnable(GL_BLEND);
                                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                                glBlendEquation(GL_FUNC_ADD);
                                glColor4f(0.2f, 0.0f, 1.0f, 0.3f);
                                draw_rectangle(r.x, r.y, r.x2 - r.x, r.y2 - r.y);
                                glDisable(GL_BLEND);
                            }
                        }
                    }
                }

                /* draw the scrollbar */
                set_color(COLOR_INDEX_SCROLLBAR_BACKGROUND);
                draw_rectangle(window_width - scrollbar_width, 0, scrollbar_width, window_height);

                update_scrollbar();

                if (scrollbar_thumb_hover)
                {
                    set_color(COLOR_INDEX_SCROLLBAR_THUMB_HOVER);
                }
                else
                {
                    set_color(COLOR_INDEX_SCROLLBAR_THUMB);
                }

                draw_rectangle(window_width - scrollbar_width + scrollbar_thumb_margin, scrollbar_thumb_position,
                        scrollbar_width - 1 * scrollbar_thumb_margin, scrollbar_thumb_size);
            }
            break;

        case D_SEARCH:
        default:
            {
                set_color(COLOR_INDEX_PAGE_BORDER);
                int search_height = font_char_height * 3 / 2;

                draw_rectangle_outline(window_width / 2 - search_width / 2, 100,
                        search_width, search_height);

                set_color(COLOR_INDEX_SCROLLBAR_BACKGROUND);
                draw_rectangle_outline(window_width / 2 - search_width / 2, 130,
                        search_width, results_shown_lines * search_height);

                const char *text = "Type to search...";
                if (strlen(search_term) == 0)
                {
                    set_color(COLOR_INDEX_DIM);
                    set_color(COLOR_INDEX_FOREGROUND);
                }
                else
                {
                    set_color(COLOR_INDEX_FOREGROUND);
                    text = search_term;
                }

                draw_string(text, window_width / 2 - search_width / 2 + 4, 100 + 4, doc_scale);

                /* draw search results */
                for (int i = 0; i < results_shown_lines; i++)
                {
                    int real_index = i + results_view_offset;

                    if (real_index < matches_count)
                    {
                        draw_string(manpage_names[matches[real_index].idx],
                                window_width / 2 - search_width / 2 + 4, 130 + i * search_height + 4, doc_scale);
                    }
                }

                if ((results_selected_index >= 0) && (results_selected_index < matches_count))
                {
                    set_color(COLOR_INDEX_BOLD);
                    int index_on_view = results_selected_index - results_view_offset;
                    draw_rectangle_outline(window_width / 2 - search_width / 2, 130 + index_on_view * search_height,
                            search_width, search_height);
                }

                {
                    char tmp[128];
                    sprintf(tmp, "%d matches", matches_count);
                    set_color(COLOR_INDEX_DIM);
                    draw_string(tmp, window_width / 2 - strlen(tmp) * character_width(doc_scale) / 2 + 4, 130 + 4 + results_shown_lines * search_height, doc_scale);
                }
            }
            break;
    }


    glutSwapBuffers();
}

void set_scroll_position(int new_scroll_position)
{
    int doc_height = document_height();
    new_scroll_position = clamp(new_scroll_position, 0, (doc_height - window_height) > 0 ? doc_height - window_height : 0);

    if (new_scroll_position != page->scroll_position)
    {
        page->scroll_position = new_scroll_position;
        glutPostRedisplay();
    }
}

int scrollbar_thumb_hittest(int x, int y)
{
    if ((x > (window_width - scrollbar_width)) && (y >= scrollbar_thumb_position) && (y < scrollbar_thumb_position + scrollbar_thumb_size))
        return 1;
    else
        return 0;
}

int results_hittest(int x, int y)
{
    int search_height = font_char_height * 3 / 2;

    for (int i = 0; i < results_shown_lines; i++)
    {
        if ((x >= window_width / 2 - search_width / 2) &&
                (x < window_width / 2 - search_width / 2 + search_width) &&
                (y >= 130 + i * search_height) &&
                (y < 130 + i * search_height + search_height))
        {
            return i;
        }
    }

    return -1; /* returns index or -1 */
}

link_t *link_under_cursor(int x, int y)
{
    struct manpage *p = page;

    int link_number = sb_count(p->links);
    for (int i = 0; i < link_number; i++)
    {
        recti r = p->links[i].document_rectangle;

        r.x += document_margin;
        r.x2 += document_margin;
        r.y += document_margin - page->scroll_position;
        r.y2 += document_margin - page->scroll_position;

        if (inside_recti(r, x, y))
        {
            return &p->links[i];
        }
    }

    return NULL;
}

void mouse_func(int button, int state, int x, int y)
{
    static int clicked_in_link = 0;
    static link_t link;

    switch (display_mode)
    {
        case D_MANPAGE:
            switch (button)
            {
                case 0:
                    if (state == GLUT_DOWN)
                    {
                        if (scrollbar_thumb_hittest(x, y))
                        {
                            scrollbar_dragging = 1;
                            scrollbar_thumb_mouse_down_y = y;
                            scrollbar_thumb_mouse_down_thumb_position = scrollbar_thumb_position;
                        }
                        else if (x >= (window_width - scrollbar_width))
                        {
                            // page up or down if clicked outside the thumb
                            if (y < scrollbar_thumb_position)
                            {
                                set_scroll_position(page->scroll_position - (window_height - line_height(doc_scale)));
                            }
                            else if (y >= (scrollbar_thumb_position + scrollbar_thumb_size))
                            {
                                set_scroll_position(page->scroll_position + window_height - line_height(doc_scale));
                            }
                        }
                        else
                        {
                            // see if a link has been clicked
                            link_t *l = link_under_cursor(x, y);
                            if (l)
                            {
                                clicked_in_link = 1;
                                link = *l;
                            }
                            else
                            {
                                clicked_in_link = 0;
                            }
                        }
                    }
                    else if (state == GLUT_UP)
                    {
                        scrollbar_dragging = 0;

                        if (clicked_in_link)
                        {
                            link_t *l = link_under_cursor(x, y);
                            if (l && (memcmp(&l->document_rectangle, &link.document_rectangle, sizeof(recti)) == 0)
                                    && (strcmp(l->link, link.link) == 0))
                            {
                                // follow the link in the same instance
                                open_new_page(link.link);
                            }
                        }
                    }
                    break;
                case 2: // right click
                    if (state == GLUT_UP)
                    {
                        page_back();
                    }
                    break;
                case 3:
                    if (state == GLUT_DOWN)
                    {
                        set_scroll_position(page->scroll_position - scroll_amount);
                    }
                    break;
                case 4:
                    if (state == GLUT_DOWN)
                    {
                        set_scroll_position(page->scroll_position + scroll_amount);
                    }
                    break;
            }
            break;
        case D_SEARCH:
            switch (button)
            {
                case 0:
                    if (state == GLUT_DOWN)
                    {
                    }
                    else if (state == GLUT_UP)
                    {
                        int index = results_hittest(x, y);
                        if (index >= 0)
                        {
                            int actual_index = index + results_view_offset;

                            if (actual_index < matches_count)
                            {
                                results_selected_index = actual_index;
                                const char *key = manpage_names[matches[results_selected_index].idx];
                                char *test;
                                if (hashmap_get(manpage_database, key, strlen(key), (void **)&test) == MAP_OK)
                                {
                                    open_new_page(test);
                                }
                            }
                        }
                    }
                    break;
                case 2: // right click
                    break;
                case 3:
                    if (state == GLUT_DOWN)
                    {
                        int index = results_hittest(x, y);
                        if (index >= 0)
                        {
                            if (results_view_offset > 0)
                            {
                                results_view_offset--;
                                int actual_index = index + results_view_offset;

                                if (actual_index < matches_count)
                                    results_selected_index = actual_index;

                                glutPostRedisplay();
                            }
                        }
                    }
                    break;
                case 4:
                    if (state == GLUT_DOWN)
                    {
                        int index = results_hittest(x, y);
                        if (index >= 0)
                        {
                            if (results_view_offset < (matches_count - results_shown_lines))
                            {
                                results_view_offset++;
                                int actual_index = index + results_view_offset;

                                if (actual_index < matches_count)
                                    results_selected_index = actual_index;

                                glutPostRedisplay();
                            }
                        }
                    }
                    break;
            }
            break;
    }
}

void mouse_motion_func(int x, int y)
{
    if (display_mode == D_SEARCH)
    {
        return;
    }

    //printf("Motion %d %d\n", x, y);
    if (scrollbar_dragging)
    {
        int new_thumb_position = clamp(scrollbar_thumb_mouse_down_thumb_position + y - scrollbar_thumb_mouse_down_y, 0, window_height - scrollbar_thumb_size);
        int new_scroll_position = scrollbar_thumb_position_to_scroll_position(new_thumb_position);
        set_scroll_position(new_scroll_position);
    }
}

void mouse_passive_motion_func(int x, int y)
{
    //printf("Passive motion %d %d\n", x, y);
    int redisplay = 0;

    switch (display_mode)
    {
        case D_MANPAGE:
            {
                if (scrollbar_thumb_hittest(x, y))
                {
                    if (scrollbar_thumb_hover == 0)
                    {
                        scrollbar_thumb_hover = 1;
                        redisplay = 1;
                    }
                }
                else
                {
                    if (scrollbar_thumb_hover == 1)
                    {
                        scrollbar_thumb_hover = 0;
                        redisplay = 1;
                    }
                }

                // check if any links reside under the mouse cursor
                struct manpage *p = page;

                int link_number = sb_count(p->links);
                for (int i = 0; i < link_number; i++)
                {
                    recti r = p->links[i].document_rectangle;

                    r.x += document_margin;
                    r.x2 += document_margin;
                    r.y += document_margin - page->scroll_position;
                    r.y2 += document_margin - page->scroll_position;

                    if (inside_recti(r, x, y))
                    {
                        if (p->links[i].highlight == 0)
                        {
                            p->links[i].highlight = 1;
                            redisplay = 1;
                        }
                    }
                    else
                    {
                        if (p->links[i].highlight > 0)
                        {
                            p->links[i].highlight = 0;
                            redisplay = 1;
                        }
                    }
                }
            }
            break;
        case D_SEARCH:
            {
                int index = results_hittest(x, y);
                if (index >= 0)
                {
                    int actual_index = index + results_view_offset;

                    if (actual_index < matches_count)
                    {
                        if (results_selected_index != actual_index)
                        {
                            results_selected_index = actual_index;
                            redisplay = 1;
                        }
                    }
                }
            }
            break;
    }

    if (redisplay > 0)
        glutPostRedisplay();
}

void keyboard_func(unsigned char key, int x, int y)
{
    static int g_pending = 0;

    if (display_mode == D_MANPAGE)
    {
        if (key == 'g')
        {
            if (g_pending)
            {
                set_scroll_position(0);
                g_pending = 0;
                return;
            }

            g_pending = 1;
            return;
        }

        if (g_pending)
        {
            g_pending = 0;
            return;
        }

        switch (key)
        {
            case 3: /* ctrl-c */
            case 4: /* ctrl-d */
            case 27: /* escape */
            case 'q':
            case 'Q':
                exit(EXIT_SUCCESS);
            case 'b':
                page_back();
                break;
            case 6: /* ctrl-f */
                display_mode = D_SEARCH;
                glutPostRedisplay();
                break;
            case 'f':
                page_forward();
                break;
            case 'i':
                doc_scale *= 1.1;
                glutReshapeWindow(fitting_window_width(doc_scale), window_height);
                break;
            case 'o':
                doc_scale *= 0.9;
                glutReshapeWindow(fitting_window_width(doc_scale), window_height);
                break;
            case 'k':
                set_scroll_position(page->scroll_position - scroll_amount);
                break;
            case 'j':
                set_scroll_position(page->scroll_position + scroll_amount);
                break;
            case 'G':
                set_scroll_position(1000000000);
                break;
            case ' ':
                {
                    int mod = glutGetModifiers();

                    if (mod & GLUT_ACTIVE_SHIFT)
                        set_scroll_position(page->scroll_position - (window_height - line_height(doc_scale)));
                    else
                        set_scroll_position(page->scroll_position + window_height - line_height(doc_scale));
                }
                break;
            default:
                break;
        }
    }
    else if (display_mode == D_SEARCH)
    {
        switch (key)
        {
            case 3: /* ctrl-c */
            case 4: /* ctrl-d */
            case 27: /* escape */
                exit(EXIT_SUCCESS);
            case '\n':
            case '\r':
                /* open selected manpage */
                if (results_selected_index < matches_count)
                {
                    const char *key = manpage_names[matches[results_selected_index].idx];
                    char *test;
                    if (hashmap_get(manpage_database, key, strlen(key), (void **)&test) == MAP_OK)
                    {
                        open_new_page(test);
                    }
                }
                break;
            case '\b':
                {
                    int len = strlen(search_term);
                    if (len > 0)
                    {
                        search_term[len - 1] = 0;
                        update_search();
                        glutPostRedisplay();
                    }
                }
                break;
            default:
                strcat(search_term, (char[]){key, 0});
                update_search();
                glutPostRedisplay();
                break;
        }
    }
}

void special_func(int key, int x, int y)
{
    switch (display_mode)
    {
        case D_MANPAGE:
            switch (key)
            {
                case GLUT_KEY_UP:
                    set_scroll_position(page->scroll_position - scroll_amount);
                    break;
                case GLUT_KEY_DOWN:
                    set_scroll_position(page->scroll_position + scroll_amount);
                    break;
                case GLUT_KEY_PAGE_UP:
                    set_scroll_position(page->scroll_position - (window_height - line_height(doc_scale)));
                    break;
                case GLUT_KEY_PAGE_DOWN:
                    set_scroll_position(page->scroll_position + window_height - line_height(doc_scale));
                    break;
                case GLUT_KEY_HOME:
                    set_scroll_position(0);
                    break;
                case GLUT_KEY_END:
                    set_scroll_position(1000000000);
                    break;
                default:
                    break;
            }
            break;
        case D_SEARCH:
            switch (key)
            {
                case GLUT_KEY_UP:
                    if (results_selected_index > 0)
                    {
                        results_selected_index--;
                        if (results_selected_index < results_view_offset)
                            results_view_offset = results_selected_index;

                        glutPostRedisplay();
                    }
                    break;
                case GLUT_KEY_DOWN:
                    if (results_selected_index < (matches_count - 1))
                    {
                        results_selected_index++;
                        if (results_selected_index > (results_view_offset + results_shown_lines - 1))
                            results_view_offset = results_selected_index - results_shown_lines + 1;

                        glutPostRedisplay();
                    }
                    break;
                case GLUT_KEY_HOME:
                    //set_scroll_position(0);
                    break;
                case GLUT_KEY_END:
                    //set_scroll_position(1000000000);
                    break;
                default:
                    break;
            }
            break;
    }
}

static int fs_lookup(const char *path, const char *sec, const char *name, char *filename_out)
{
    struct stat sb;
    glob_t globinfo;
    char file[1024];

    sprintf(file, "%s/man%s/%s.%s", path, sec, name, sec);
    if (stat(file, &sb) != -1)
        goto found;

    sprintf(file, "%s/cat%s/%s.0", path, sec, name);
    if (stat(file, &sb) != -1)
        goto found;

#if 0
    if (arch != NULL) {
        sprintf(file, "%s/man%s/%s/%s.%s", path, sec, arch, name, sec);
        if (stat(file, &sb) != -1)
            goto found;
    }
#endif

    sprintf(file, "%s/man%s/%s.[01-9]*", path, sec, name);
    int globres = glob(file, 0, NULL, &globinfo);
    if (globres != 0 && globres != GLOB_NOMATCH)
        warn("%s: glob", file);

    if (globres == 0)
        strcpy(file, *globinfo.gl_pathv);
    globfree(&globinfo);

    if (globres == 0) {
        if (stat(file, &sb) != -1)
            goto found;
    }

    sprintf(file, "%s.%s", name, sec);
    globres = stat(file, &sb);
    if (globres != -1)
    {
        goto found;
    }

    return -1;

found:
    strcpy(filename_out, file);
    return 0;
}

static int search_filesystem(const char *section, const char *search_term, char *filename_out)
{
    const char * const sections[] = {"1", "8", "6", "2", "3", "5", "7", "4", "9", "3p"};

    const char * const paths[] =
    {
        "/usr/share/man",
        "/usr/X11R6/man",
        "/usr/local/man"
    };

    size_t ipath, isec;

    for (ipath = 0; ipath < ARRAY_SIZE(paths); ipath++)
    {
        if (section)
        {
            if (fs_lookup(paths[ipath], section, search_term, filename_out) == 0)
                return 0;
        }
        else
        {
            for (isec = 0; isec < ARRAY_SIZE(sections); isec++)
            {
                if (fs_lookup(paths[ipath], sections[isec], search_term, filename_out) == 0)
                    return 0;
            }
        }
    }

    if (section == NULL)
        fprintf(stderr, "No entry for %s in the manual.\n", search_term);
    else
        fprintf(stderr, "No entry for %s in section %s of the manual.\n", search_term, section);

    return -1;
}

int ends_with_ignore_case(const char *str, const char *ending)
{
    int len_ending = strlen(ending);
    int len_str = strlen(str);
    if (len_str >= len_ending)
        return strcasecmp(&str[len_str - len_ending], ending);

    return -1;
}

int get_page_name_and_section(const char *pathname, char *name, char *section)
{
    int len = strlen(pathname);
    if (len > 0)
    {
        int last_slash_index = -1;
        int i = len - 1;
        while (i >= 0)
        {
            if (pathname[i] == '/')
            {
                last_slash_index = i;
                break;
            }
            i--;
        }

        char filename[256];
        strcpy(filename, &pathname[last_slash_index + 1]);

        {
            if (ends_with_ignore_case(filename, ".gz") == 0)
            {
                // remove the .gz ending
                filename[strlen(filename) - 3] = 0;
            }

            int len = strlen(filename);
            int i = len - 1;

            while (i >= 0)
            {
                if (filename[i] == '.')
                {
                    strcpy(section, &filename[i + 1]);
                    memcpy(name, filename, i);
                    name[i] = 0;
                    return 0;
                }

                i--;
            }
        }
    }

    return -1;
}

static int make_manpage_database(void)
{
    const char * const sections[] = {"1", "8", "6", "2", "3", "5", "7", "4", "9", "3p"};

    const char * const paths[] =
    {
        "/usr/share/man",
        "/usr/X11R6/man",
        "/usr/local/man"
    };

    size_t ipath, isec;

    for (ipath = 0; ipath < ARRAY_SIZE(paths); ipath++)
    {
        for (isec = 0; isec < ARRAY_SIZE(sections); isec++)
        {
            const char *path = paths[ipath];
            const char *section = sections[isec];

            glob_t globinfo;
            char file[1024];

            sprintf(file, "%s/man%s/*.[01-9]*", path, section);
            sprintf(file, "%s/man%s/*", path, section);
            int globres = glob(file, 0, NULL, &globinfo);
            if (globres != 0 && globres != GLOB_NOMATCH)
                warn("%s: glob", file);

            if (globres == 0)
            {
                // there are matches
                for (int i = 0; i < globinfo.gl_pathc; i++)
                {
                    //printf("%s\n", globinfo.gl_pathv[i]);

                    char page_name[512];
                    char section_name[3];
                    if (get_page_name_and_section(globinfo.gl_pathv[i], page_name, section_name) == 0)
                    {
                        // successful parse
                        char key[512];
                        sprintf(key, "%s(%s)", page_name, section_name);

                        char *file = strdup(globinfo.gl_pathv[i]);
                        char *test;
                        if (hashmap_get(manpage_database, key, strlen(key), (void **)&test) == MAP_OK)
                        {
                            //printf("Key present, removing\n");
                            hashmap_remove(manpage_database, key, strlen(key));
                            free(test);
                        }
                        hashmap_put(manpage_database, key, strlen(key), file);
                        sb_push(manpage_names, strdup(key));
                    }
                }
            }

            globfree(&globinfo);
#if 0
            sprintf(file, "%s/man%s/%s.%s", path, sec, name, sec);
            if (stat(file, &sb) != -1)
                goto found;

            sprintf(file, "%s/cat%s/%s.0", path, sec, name);
            if (stat(file, &sb) != -1)
                goto found;

#if 0
            if (arch != NULL) {
                sprintf(file, "%s/man%s/%s/%s.%s", path, sec, arch, name, sec);
                if (stat(file, &sb) != -1)
                    goto found;
            }
#endif

            sprintf(file, "%s/man%s/%s.[01-9]*", path, sec, name);
            int globres = glob(file, 0, NULL, &globinfo);
            if (globres != 0 && globres != GLOB_NOMATCH)
                warn("%s: glob", file);

            if (globres == 0)
                strcpy(file, *globinfo.gl_pathv);
            globfree(&globinfo);

            if (globres == 0) {
                if (stat(file, &sb) != -1)
                    goto found;
            }
#endif


        }
    }

    return 0;
}

struct manpage *load_manpage(const char *filename)
{
    mchars_alloc(); // initialize charset table

    struct mparse *parse = mparse_alloc(MPARSE_SO | MPARSE_UTF8 | MPARSE_LATIN1 | MPARSE_VALIDATE /*options=autodetect document type*/,
            MANDOC_OS_OTHER /*mandoc_os = automatically detect*/,
            NULL /*os_s = string passed to override the result of uname*/);

    mandoc_msg_setinfilename(filename);
    mandoc_msg_setoutfile(stderr);

    int fd = mparse_open(parse, filename); // open a file and if it fails try appending .gz

    if (fd == -1)
    {
        fprintf(stderr, "Failed to open file %s (%s)\n", filename, strerror(errno));
        exit(1);
    }

    mparse_readfd(parse, fd, filename);

    close(fd);

    struct roff_meta *meta = mparse_result(parse);

    struct manpage *page = ZMALLOC(struct manpage, 1);

    strcpy(page->filename, filename);
    get_page_name_and_section(filename, page->manpage_name, page->manpage_section);

    add_line(page);

    void *formatter = mangl_formatter(78, 5);
    formatting_page = page; // temporary use of a global variable for formatting functions (not multithreaded)

    if (meta->macroset == MACROSET_MDOC)
    {
        terminal_mdoc(formatter, meta); // for mdoc format
    }
    else
    {
        terminal_man(formatter, meta);
    }

    formatting_page = NULL;

    /* remove the last line empty line */
    if (page->document.n_lines > 1)
    {
        struct span *s = page->document.lines[page->document.n_lines - 1];
        if (((s->buffer == NULL) || (strlen(s->buffer) == 0)) && (s->next == NULL))
        {
            page->document.n_lines--;
            page->document.lines[page->document.n_lines /* already decremented */] = NULL;
            free_span(s);
        }
    }

    find_links(page); // update links

    mangl_formatter_free(formatter);
    mparse_free(parse);
    mchars_free();

    return page;
}

void update_window_title()
{
    char window_title[256];

    if (strlen(page->manpage_name) > 0)
    {
        sprintf(window_title, "%s(%s) - mangl", page->manpage_name, page->manpage_section);
    }
    else
    {
        sprintf(window_title, "%s - mangl", page->filename);
    }

    glutSetWindowTitle(window_title);
}

void open_new_page(const char *filename)
{
    struct manpage *new_page = load_manpage(filename);

    // put on stack
    if (stack_pos < sb_count(page_stack))
    {
        // additional pages on stack, need NULLing
        for (int i = stack_pos; i < sb_count(page_stack); i++)
        {
            if (page_stack[i])
            {
                free_manpage(page_stack[i]);
                page_stack[i] = NULL;
            }
        }

        stack_pos++;
        page_stack[stack_pos - 1] = new_page;
    }
    else
    {
        sb_push(page_stack, new_page);
        stack_pos++;
    }

    page = new_page;
    if (display_mode == D_SEARCH)
        display_mode = D_MANPAGE;
    update_window_title();
    update_scrollbar();
    glutPostRedisplay();
}

void page_back(void)
{
    if (stack_pos > 1)
    {
        stack_pos--;
        page = page_stack[stack_pos - 1];
        update_window_title();
        update_scrollbar();
        glutPostRedisplay();
    }
}

void page_forward(void)
{
    if ((stack_pos < sb_count(page_stack)) && page_stack[stack_pos])
    {
        stack_pos++;
        page = page_stack[stack_pos - 1];
        update_window_title();
        update_scrollbar();
        glutPostRedisplay();
    }
}

int main(int argc, char *argv[])
{
    char window_title[256];
    manpage_database = hashmap_new();

    make_manpage_database();

    if (argc < 2)
    {
        display_mode = D_SEARCH;
        strcpy(window_title, "mangl");
    }
    else
    {
        const char *filename = NULL;
        char tmp_filename[1024];

        if ((strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "--help") == 0))
        {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        else if (strcmp(argv[1], "-f") == 0)
        {
            if (argc >= 3)
            {
                filename = argv[2];
            }
            else
            {
                fprintf(stderr, "Argument required after \"-f\"\n");
                print_usage(argv[0]);
            }
        }
        else
        {
            const char *section = NULL;
            const char *search_term = NULL;
            if (argc == 2)
            {
                search_term = argv[1];
            }
            else if (argc == 3)
            {
                section = argv[1];
                search_term = argv[2];
            }

            if (search_filesystem(section, search_term, tmp_filename) == 0)
            {
                filename = tmp_filename;
            }
        }

        if (filename == NULL)
            exit(EXIT_FAILURE);

        page = load_manpage(filename);

        sb_push(page_stack, page);
        stack_pos++;

        if (strlen(page->manpage_name) > 0)
        {
            sprintf(window_title, "%s(%s) - mangl", page->manpage_name, page->manpage_section);
        }
        else
        {
            sprintf(window_title, "%s - mangl", page->filename);
        }
    }

    /* display gui */
    if (fork() != 0)
    {
        exit(EXIT_SUCCESS);
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(0);
    //glutInitDisplayMode(GLUT_DOUBLE);
    glutInitWindowSize(fitting_window_width(doc_scale), 600);

    glutCreateWindow(window_title);

    glutReshapeFunc(&reshape);
    glutDisplayFunc(&render);

    glutMouseFunc(&mouse_func);
    glutMotionFunc(&mouse_motion_func);
    glutPassiveMotionFunc(&mouse_passive_motion_func);
    glutKeyboardFunc(&keyboard_func);
    glutSpecialFunc(&special_func);

    init_gl();

    glutMainLoop();

    if (page)
        free_manpage(page);

    return 0;
}

