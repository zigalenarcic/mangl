/*
 * main.c
 *
 * Implementation of 'mangl' - a graphical man page viewer
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

#include "ft2build.h"
#include FT_FREETYPE_H

#include "stretchy_buffer.h"
#include "hashmap.h"

#include "mandoc/mandoc.h"
#include "mandoc/roff.h"
#include "mandoc/mandoc_parse.h"
#include "mandoc/manconf.h"
#include "mandoc/out.h"
#include "mandoc/mandoc_aux.h"
#include "mandoc/term.h"

#define MANGL_VERSION_MAJOR 1
#define MANGL_VERSION_MINOR 0

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
#define ZMALLOC(type, n) ((type *)calloc(n, sizeof(type)))
#define MAX(x,y) (((x) > (y)) ? (x) : (y))
#define MIN(x,y) (((x) < (y)) ? (x) : (y))

enum DISPLAY_MODES {
    D_MANPAGE = 0,
    D_SEARCH = 1
};

enum DIMENSIONS {
    DIM_SCROLLBAR_WIDTH = 0,
    DIM_SCROLLBAR_THUMB_MARGIN,
    DIM_SCROLLBAR_THUMB_MIN_HEIGHT,
    DIM_DOCUMENT_MARGIN,
    DIM_SEARCH_WIDTH,
    DIM_SCROLL_AMOUNT,
    DIM_GUI_PADDING,
    DIM_TEXT_HORIZONTAL_MARGIN,
};

static const int dimensions[] =
{
/*scrollbar_width        */ 12,
/*scrollbar_thumb_margin */ 0,
/*scrollbar_thumb_min_height*/ 20,
/*document_margin        */ 29,
/*search_width           */ 300,
/*scroll amount          */ 40,
/*gui padding            */ 9,
/*text horizontal margin */ 4,
};

#define FONT_TEXTURE_SIZE 256
const static unsigned int font_image_width = 112;
const static unsigned int font_image_height = 84;
const static unsigned int font_char_width = 7;
const static unsigned int font_char_height = 14;
#include "font_image.h"

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

typedef struct {
    recti document_rectangle;
} search_t;

typedef struct CharDescription_
{
    int available;
    float tex_coord0_x;
    float tex_coord0_y;
    float tex_coord1_x;
    float tex_coord1_y;

    int width;
    int height;
    int top;
    int left;
    int advance;
} CharDescription;

typedef struct FontData_
{
    uint8_t *bitmap;
    int bitmap_width;
    int bitmap_height;
    CharDescription chars[128];
    int character_width;
    int character_height;
    int line_height;
    double font_size;
    GLuint texture_id;
} FontData;

FontData builtinFont = {
    .bitmap = NULL,
    .bitmap_width = 0,
    .bitmap_height = 0,
    .character_width = 6, // actual width and height
    .character_height = 9,
    .line_height = font_char_height,
    .font_size = 10.0,
    .texture_id = 0
};

FontData *mainFont = &builtinFont;

FontData *loadedFont;

struct {
    char font_file[512];
    int font_size;
    double gui_scale;
    double line_spacing;
} settings = { .font_size = 10, .gui_scale = 1.0, .line_spacing = 1.0};

int display_mode = D_MANPAGE;
char search_term[512];

char **manpage_names;

#define N_SHOWN_RESULTS 12
int results_selected_index = 0;
int results_shown_lines = N_SHOWN_RESULTS;
int results_view_offset = 0;

struct {
    int idx;
    int goodness;
} matches[100];

int matches_count = 0;

int scrollbar_thumb_position;
int scrollbar_thumb_size;
int scrollbar_thumb_hover;

int scrollbar_dragging = 0;
int scrollbar_thumb_mouse_down_y = 0;
int scrollbar_thumb_mouse_down_thumb_position = 0;

int initial_window_rows = 40;

int window_width;
int window_height;

map_t manpage_database;

FT_Library library;

void terminal_mdoc(void *, const struct roff_meta *);
void terminal_man(void *, const struct roff_meta *);

double clamp(double val, double min, double max)
{
    if (val > max) return max;
    if (val < min) return min;
    return val;
}

bool inside_recti(recti r, int x, int y)
{
    return (x >= r.x) && (y >= r.y) && (x < r.x2) && (y < r.y2);
}

unsigned round_to_power_of_2(unsigned x)
{
    for (int i = 31; i >= 0; i--)
    {
        if (x == (1 << i))
            return 1 << i;

        if ((x & (1 << i)) != 0)
            return 1 << (i + 1);
    }

    return 0;
}

int get_font_file(char *font)
{
    FILE *f = fopen(font, "rb");

    if (f)
    {
        fclose(f);
        return 1;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "fc-match --format=%%{file} \"%s\"", font);

    FILE *proc = popen(cmd, "r");

    int proc_fd = fileno(proc);
    /* wait for output */

    fd_set fd_s;
    FD_ZERO(&fd_s);
    FD_SET(proc_fd, &fd_s);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100 * 1000;

    int ret = select(proc_fd + 1, &fd_s, NULL, NULL, &timeout);

    if (ret > 0)
    {
        /* read the output */
        if (proc != NULL)
        {
            char tmp_out[1024];
            char *ptr = fgets(tmp_out, sizeof(tmp_out), proc);
            pclose(proc);

            if (ptr)
            {
                strcpy(font, ptr);
                return 1;
            }
        }
    }

    return 0;
}

void copy_bitmap(uint8_t *dst, int w_dst, int h_dst, int x, int y, const uint8_t *src, int w_src, int h_src, int pitch_src)
{
    for (int j = 0; j < h_src; j++)
    {
        if ((j + y) >= h_dst)
            break;

        int bytes_to_copy = w_src;
        if ((bytes_to_copy + x) > w_dst)
        {
            bytes_to_copy = w_dst - x; // clip
        }

        memcpy(&dst[(j + y) * w_dst + x], &src[j * pitch_src], bytes_to_copy);
    }
}

void copy_bitmap_1bit(uint8_t *dst, int w_dst, int h_dst, int x, int y, const uint8_t *src, int w_src, int h_src, int pitch_src)
{
    for (int j = 0; j < h_src; j++)
    {
        if ((j + y) >= h_dst)
            break;

        for (int i = 0; i < w_src; i++)
        {
            if ((x + i) >= w_dst)
                break;
            int byte = i / 8;
            int bit = 7 - (i % 8);
            dst[(j + y) * w_dst + x + i] = (src[j * pitch_src + byte] & (1U << bit)) ? 255 : 0;
        }
    }
}

int get_dimension(int dimension_index)
{
    /* at 10 pt font is 6x9 pixels */
    switch (dimension_index)
    {
        case DIM_DOCUMENT_MARGIN:
        case DIM_SCROLL_AMOUNT:
        case DIM_GUI_PADDING:
            {
                double font_scale = mainFont->character_height / 9.0;
                return (int)(font_scale * dimensions[dimension_index]);
            }

        case DIM_SEARCH_WIDTH:
        case DIM_TEXT_HORIZONTAL_MARGIN:
            {
                double font_horizontal_scale = mainFont->character_width / 6.0;
                return (int)(font_horizontal_scale * dimensions[dimension_index]);
            }

        case DIM_SCROLLBAR_WIDTH:
        case DIM_SCROLLBAR_THUMB_MARGIN:
        case DIM_SCROLLBAR_THUMB_MIN_HEIGHT:
        default:
            return (int)(settings.gui_scale * dimensions[dimension_index]);

    }
}

int init_freetype(void)
{
    int error = FT_Init_FreeType(&library);
    if (error)
    {
        fprintf(stderr, "Failed to initialize freetype\n");
        exit(EXIT_FAILURE);
    }
    return 0;
}

int render_font_texture(const char *font_file, int font_size_px)
{
    FT_Face face;

    FILE *f = fopen(font_file, "rb");
    if (f == NULL)
    {
        fprintf(stderr, "Font file missing: \"%s\"\n", font_file);
        return -1;
    }
    else
    {
        fclose(f);
    }

    int error = FT_New_Face(library, font_file, 0,  &face);
    if (error == FT_Err_Unknown_File_Format)
    {
        fprintf(stderr, "Unknown font format: %s\n", font_file);
        return -1;
    }
    else if (error)
    {
        fprintf(stderr, "File not found: %s\n", font_file);
        return -1;
    }
    else
    {
        //printf("Font \"%s\" loaded\n", font_file);
    }

    //error = FT_Set_Pixel_Sizes(face, 0, font_size_px);
    error = FT_Set_Char_Size(face, 0, font_size_px * 64, 96, 96);
    if (error)
    {
        fprintf(stderr, "FT_Set_Char_Size error: %d\n", error);
        return -1;
    }

    int font_width = 0;
    int font_height = 0;

    /* get character size of X, H */
    const char *str = "XH";
    while (*str)
    {
        int glyph_index = FT_Get_Char_Index(face, *str);
        str++;
        if (glyph_index > 0)
        {
            FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
            error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
            if (error)
            {
                fprintf(stderr, "FT_Render_Glyph: Glyph render error: %d\n", error);
            }

            FT_Bitmap *bmp = &face->glyph->bitmap;

            font_width = MAX(font_width, bmp->width);
            font_height = MAX(font_height, bmp->rows);
        }
    }

    if ((font_width <= 0) || (font_height <= 0))
    {
        fprintf(stderr, "Failed to determine font size\n");
        return -1;
    }

    /* now allocate a texture of a good size */

    FontData *font = ZMALLOC(FontData, 1);

    font->font_size = font_size_px;
    font->character_width = font_width;
    font->character_height = font_height;
    font->line_height = face->size->metrics.height / 64 + 1; /* add 1 px of line height to make text more breathy */

    font->bitmap_width = round_to_power_of_2(16 * (font_width + 2));
    font->bitmap_height = round_to_power_of_2(6 * (font_height * 2));
    font->bitmap_width = MAX(font->bitmap_width, font->bitmap_height);
    font->bitmap_height = MAX(font->bitmap_width, font->bitmap_height);

    font->bitmap = ZMALLOC(uint8_t, font->bitmap_width * font->bitmap_height);

    for (int i = 32; i < 128; i++)
    {
        int glyph_index = FT_Get_Char_Index(face, i);
        if (glyph_index > 0)
        {
            FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);

            error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
            if (error)
            {
                fprintf(stderr, "FT_Render_Glyph: Glyph render error: %d\n", error);
                continue;
            }

            FT_Bitmap *bmp = &face->glyph->bitmap;

            int w = bmp->width;
            int h = bmp->rows;
            int pitch = bmp->pitch;
            int left = face->glyph->bitmap_left;
            int top = face->glyph->bitmap_top;

            //printf("w %d h %d pitch %d left %d top %d\n", w, h, pitch, left, top);

            int col = i % 16;
            int row = i / 16;
            int dst_x = col * (font_width + 2) + left + 1;
            int dst_y = row * (font_height * 2) - top;


            font->chars[i].available = 1;
            font->chars[i].top = top;
            font->chars[i].left = left;
            font->chars[i].width = w;
            font->chars[i].height = h;
            font->chars[i].advance = face->glyph->advance.x / 64;

            const float pixel_x = 1.0f / font->bitmap_width;
            const float pixel_y = 1.0f / font->bitmap_height;

            font->chars[i].tex_coord0_x = pixel_x * dst_x;
            font->chars[i].tex_coord0_y = pixel_y * dst_y;
            font->chars[i].tex_coord1_x = pixel_x * (dst_x + w);
            font->chars[i].tex_coord1_y = pixel_y * (dst_y + h);

            if (bmp->pixel_mode == FT_PIXEL_MODE_GRAY)
                copy_bitmap(font->bitmap, font->bitmap_width, font->bitmap_height, dst_x, dst_y, bmp->buffer, w, h, pitch);
            else if (bmp->pixel_mode == FT_PIXEL_MODE_MONO)
            {
                // monochrome
                copy_bitmap_1bit(font->bitmap, font->bitmap_width, font->bitmap_height, dst_x, dst_y, bmp->buffer, w, h, pitch);
            }
            else
            {
                printf("Unsupported pixel mode (not 8 bit or 1 bit)\n");
            }

        }
        else
        {
            //printf("no glyph for %d\n", i);
            continue;
        }
    }

    FT_Done_Face(face);
    loadedFont = font;

    return 0;
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

    int search_start_scroll_position;
    int search_input_active;
    char search_string[256];
    int search_visible;

    search_t searches[100];
    int search_num;
    int search_index;
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
    else if ((letter == 0x2018) || (letter == 0x2019)) /* left and right single quotation mark */
        letter = '\'';

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

int get_line_advance(void)
{
    if (mainFont)
        return (int)(settings.line_spacing * mainFont->line_height);

    return 2 * font_char_height;
}

int get_line_height(void)
{
    if (mainFont)
        return (int)(mainFont->line_height);

    return 2 * font_char_height;
}

int get_character_width(void)
{
    if (mainFont)
        return mainFont->chars['X'].advance;

    return font_char_width;
}

int document_width(void)
{
    return 2 * get_dimension(DIM_DOCUMENT_MARGIN) + ((78 + 2) * get_character_width());
}

int document_height(void)
{
    return page->document.n_lines * get_line_advance() + 2 * get_dimension(DIM_DOCUMENT_MARGIN);
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
                //draw_string(s->buffer, get_dimension(DIM_DOCUMENT_MARGIN), get_dimension(DIM_DOCUMENT_MARGIN) + vertical_position - page->scroll_position);

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
                                l.document_rectangle.x = ((intptr_t)str - (intptr_t)line + 1 - strlen(current_word)) * get_character_width();
                                l.document_rectangle.y = i * get_line_advance();
                                l.document_rectangle.x2 = l.document_rectangle.x + strlen(current_word) * get_character_width();
                                l.document_rectangle.y2 = l.document_rectangle.y + get_line_height();

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

        vertical_position += get_line_advance();
    }
}

bool contains_uppercase(const char *str)
{
    while (*str)
    {
        if ((*str >= 'A') && (*str <= 'Z'))
            return true;

        str++;
    }

    return false;
}

void update_page_search(struct manpage *p)
{
    p->search_num = 0;
    p->search_index = 0;
    int search_index_set = 0;

    if (strlen(p->search_string) == 0)
        return;

    int search_len = strlen(p->search_string);

    int ignore_case = 1;

    if (contains_uppercase(p->search_string))
    {
        ignore_case = 0;
    }

    int vertical_position = 0;

    for (int i = 0; i < p->document.n_lines; i++)
    {
        struct span *s = p->document.lines[i];

        char line[2048];
        int pos = 0;
        line[0] = 0;

        while (s)
        {
            if (s->length > 0)
            {
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

            }
            s = s->next;
        }

        line[pos] = 0;

        {
            /* search the current line */
            char *str = line;

            while (*str)
            {
                if ((ignore_case && (strncasecmp(str, p->search_string, search_len) == 0)) ||
                        (strncmp(str, p->search_string, search_len) == 0))
                {
                    /* we have a match */
                    search_t *s = &p->searches[p->search_num];

                    s->document_rectangle.x = ((intptr_t)str - (intptr_t)line) * get_character_width();
                    s->document_rectangle.y = i * get_line_advance();
                    s->document_rectangle.x2 = s->document_rectangle.x + strlen(p->search_string) * get_character_width();
                    s->document_rectangle.y2 = s->document_rectangle.y + get_line_height();

                    if ((s->document_rectangle.y + get_dimension(DIM_DOCUMENT_MARGIN)) >= p->search_start_scroll_position)
                    {
                        if (search_index_set == 0)
                            p->search_index = p->search_num;
                        search_index_set = 1;
                    }

                    p->search_num++;

                    if (p->search_num >= ARRAY_SIZE(p->searches))
                    {
                        if (search_index_set == 0)
                            p->search_index = 0;
                        return;
                    }

                    str += strlen(p->search_string);
                }
                else
                {
                    str++;
                }
            }

            if (p->search_num >= ARRAY_SIZE(p->searches))
            {
                if (search_index_set == 0)
                    p->search_index = 0;
                return;
            }
        }

        vertical_position += get_line_advance();
    }
}

void update_scrollbar(void)
{
    if (display_mode == D_SEARCH)
        return;

    int doc_height = document_height();
    int thumb_size_tmp = (double)window_height / (doc_height - 1) * window_height;

    scrollbar_thumb_size = clamp(thumb_size_tmp, get_dimension(DIM_SCROLLBAR_THUMB_MIN_HEIGHT), window_height);
    //scrollbar_thumb_position = round((double)page->scroll_position / (doc_height - 1) * window_height);
    scrollbar_thumb_position = round((double)page->scroll_position / (doc_height - window_height) * (window_height - scrollbar_thumb_size));
}

int scrollbar_thumb_position_to_scroll_position(int thumb_position)
{
    int doc_height = document_height();
    int thumb_size_tmp = (double)window_height / (doc_height - 1) * window_height;

    scrollbar_thumb_size = clamp(thumb_size_tmp, get_dimension(DIM_SCROLLBAR_THUMB_MIN_HEIGHT), window_height);

    int scrollbar_height = window_height;

    double percentage = (double)thumb_position / (scrollbar_height - scrollbar_thumb_size);

    return percentage * (doc_height - window_height);
}

void reshape(int w, int h)
{
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

int fitting_window_width(void)
{
    return 2 * get_dimension(DIM_DOCUMENT_MARGIN) +
        ((78 + 2) * get_character_width()) + get_dimension(DIM_SCROLLBAR_WIDTH);
}

int fitting_window_height(int num_rows)
{
    return num_rows * get_line_advance();
}

float color_table[][3] = {
    {21.0f/255.0f, 21.0f/255.0f, 21.0f/255.0f},
    {253.0f/255.0f, 253.0f/255.0f, 232.0f/255.0f},
    {164.0f/255.0f, 212.0f/255.0f, 241.0f/255.0f},
    {255.0f/255.0f, 206.0f/255.0f, 121.0f/255.0f},
    {123.0f/255.0f, 123.0f/255.0f, 123.0f/255.0f},
    {38.0f/255.0f, 38.0f/255.0f, 38.0f/255.0f},
    {69.0f/255.0f, 69.0f/255.0f, 69.0f/255.0f},
    {84.0f/255.0f, 84.0f/255.0f, 84.0f/255.0f},
    {72.0f/255.0f, 21.0f/255.0f, 255.0f/255.0f},
    {235.0f/255.0f, 180.0f/255.0f, 112.0f/255.0f},
    {143.0f/255.0f, 191.0f/255.0f, 220.0f/255.0f},
    {255.0f/255.0f, 21.0f/255.0f, 21.0f/255.0f},
    {21.0f/255.0f, 21.0f/255.0f, 180.0f/255.0f},
    {21.0f/255.0f, 200.0f/255.0f, 21.0f/255.0f},
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
    COLOR_INDEX_LINK,
    COLOR_INDEX_GUI_1, /* amber */
    COLOR_INDEX_GUI_2, /* blue */
    COLOR_INDEX_ERROR, /* red-like */
    COLOR_INDEX_SEARCHES,
    COLOR_INDEX_SEARCH_SELECTED,
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
    glTranslatef(0.5, 0.5, 0); /* fix missing pixel in the corner */
    w -= 1; /* to match normal quads */
    h -= 1;
    glBegin(GL_LINE_STRIP);
    glVertex2i(x, y);
    glVertex2i(x + w, y);
    glVertex2i(x + w, y + h);
    glVertex2i(x, y + h);
    glVertex2i(x, y);
    glEnd();
    glTranslatef(-0.5, -0.5, 0);
}

/*
   !"#$%&'()*+,-./
0123456789:;<=>?
@ABCDEFGHIJKLMNO
PQRSTUVWXYZ[\]^_
`abcdefghijklmno
pqrstuvwxyz{|}~
*/

int put_char_gl(int x, int y, char c)
{
    int ret = 0;
    int w = font_char_width;
    int h = font_char_height;

    glBindTexture(GL_TEXTURE_2D, mainFont->texture_id);
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
#if 0
        static const float pixel = 1.0f / FONT_TEXTURE_SIZE;

        c -= 32;

        char c_line = c / 16;
        char c_pos = c % 16;

        glBegin(GL_QUADS);
        glTexCoord2f(c_pos * pixel * w, c_line * h * pixel); glVertex2f(x, y);
        glTexCoord2f(c_pos * pixel * w, (c_line + 1) * h * pixel); glVertex2f(x, y + h);
        glTexCoord2f((c_pos + 1) * pixel * w, (c_line + 1) * h * pixel); glVertex2f(x + w, y + h);
        glTexCoord2f((c_pos + 1) * pixel * w, c_line * h * pixel); glVertex2f(x + w, y);
        glEnd();

        ret = w;
#else
        int idx = (int)c;
        if (mainFont->chars[idx].available)
        {
            int w = mainFont->chars[idx].width;
            int h = mainFont->chars[idx].height;
            int x_start = x + mainFont->chars[idx].left;
            int y_start = y - mainFont->chars[idx].top + mainFont->character_height + 2;

            glBegin(GL_QUADS);
            glTexCoord2f(mainFont->chars[idx].tex_coord0_x, mainFont->chars[idx].tex_coord0_y);
            glVertex2f(x_start, y_start);
            glTexCoord2f(mainFont->chars[idx].tex_coord0_x, mainFont->chars[idx].tex_coord1_y);
            glVertex2f(x_start, y_start + h);
            glTexCoord2f(mainFont->chars[idx].tex_coord1_x, mainFont->chars[idx].tex_coord1_y);
            glVertex2f(x_start + w, y_start + h);
            glTexCoord2f(mainFont->chars[idx].tex_coord1_x, mainFont->chars[idx].tex_coord0_y);
            glVertex2f(x_start + w, y_start);
            glEnd();

            ret = mainFont->chars[idx].advance;
        }
        else
        {
            glDisable(GL_BLEND);
            draw_rectangle_outline(x + 1, y + 1, w - 2, h - 2);
            glEnable(GL_BLEND);
            ret = mainFont->character_width;
        }
#endif
    }

    glDisable(GL_BLEND);

    return ret;
}

void print_text_gl(int x, int y, const char *str)
{
    while (*str)
    {
        x += put_char_gl(x, y, *str);
        str++;
    }
}

size_t draw_string_manpage(const char *str, int x, int y)
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

        count++;
        x += put_char_gl(x, y, *str);

        if (color_set)
        {
            set_color(COLOR_INDEX_FOREGROUND);
        }
        str++;
    }

    return count;
}

size_t draw_string(const char *str, int x, int y)
{
    size_t count = 0;
    while (*str)
    {
        count++;
        x += put_char_gl(x, y, *str);
        str++;
    }

    return count;
}

void add_gl_texture_monochrome(GLuint *texture, int width, int height, void *data)
{
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}


void init_builtin_font(void)
{
    builtinFont.bitmap = ZMALLOC(uint8_t, FONT_TEXTURE_SIZE * FONT_TEXTURE_SIZE);
    builtinFont.bitmap_width = FONT_TEXTURE_SIZE;
    builtinFont.bitmap_height = FONT_TEXTURE_SIZE;

    /* copy font_image into a larger texture 2^n sized buffer */
    copy_bitmap(builtinFont.bitmap, builtinFont.bitmap_width, builtinFont.bitmap_height, 0, 0,
            font_image, font_image_width, font_image_height, font_image_width);

    /* fill information for builtin font */
    for (int i = 32; i < 128; i++)
    {
        builtinFont.chars[i].available = 1;
        builtinFont.chars[i].top = builtinFont.character_height; /* actual height of X character in px */
        builtinFont.chars[i].left = 0;
        builtinFont.chars[i].width = font_char_width;
        builtinFont.chars[i].height = font_char_height;
        builtinFont.chars[i].advance = font_char_width;

        float pixel_x = 1.0f / builtinFont.bitmap_width;
        float pixel_y = 1.0f / builtinFont.bitmap_height;

        int c_col = (i - 32) % 16;
        int c_row = (i - 32) / 16;

        builtinFont.chars[i].tex_coord0_x = pixel_x * c_col * font_char_width;
        builtinFont.chars[i].tex_coord0_y = pixel_y * c_row * font_char_height;
        builtinFont.chars[i].tex_coord1_x = pixel_x * (c_col + 1) * font_char_width;
        builtinFont.chars[i].tex_coord1_y = pixel_y * (c_row + 1) * font_char_height;
    }
}

void upload_font_textures(void)
{
    add_gl_texture_monochrome(&builtinFont.texture_id, builtinFont.bitmap_width, builtinFont.bitmap_height, builtinFont.bitmap);

    if (loadedFont)
    {
        add_gl_texture_monochrome(&loadedFont->texture_id, loadedFont->bitmap_width, loadedFont->bitmap_height, loadedFont->bitmap);
    }
}

void render_manpage(struct manpage *p)
{
    int vertical_position = 0;

    for (int i = 0; i < p->document.n_lines; i++)
    {
        struct span *s = p->document.lines[i];

        if ((vertical_position >= (page->scroll_position - get_line_advance() - get_dimension(DIM_DOCUMENT_MARGIN))) &&
                ((vertical_position - get_line_advance()) < (page->scroll_position + window_height)))
        {
            int num_chars = 0;
            while (s)
            {
                if (s->length > 0)
                {
                    num_chars += draw_string_manpage(s->buffer,
                            get_dimension(DIM_DOCUMENT_MARGIN) + num_chars * get_character_width(),
                            get_dimension(DIM_DOCUMENT_MARGIN) + vertical_position - page->scroll_position);
                }
                s = s->next;
            }
        }

        vertical_position += get_line_advance();

        if ((vertical_position - get_line_advance()) > (page->scroll_position + window_height))
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
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_BLEND);

    switch (display_mode)
    {
        case D_MANPAGE:
            {
                /* draw document border */
                int border_margin = get_dimension(DIM_DOCUMENT_MARGIN) * 3 / 8 + 1;
                set_color(COLOR_INDEX_GUI_1);
                draw_rectangle_outline(border_margin, border_margin - page->scroll_position,
                        document_width() - 2 * border_margin, document_height() - 2 * border_margin);

                /* draw page search matches */
                if (page->search_visible)
                {
                    for (int i = 0; i < page->search_num; i++)
                    {
                        recti r = page->searches[i].document_rectangle;

                        r.x += get_dimension(DIM_DOCUMENT_MARGIN);
                        r.x2 += get_dimension(DIM_DOCUMENT_MARGIN);
                        r.y += get_dimension(DIM_DOCUMENT_MARGIN) - page->scroll_position;
                        r.y2 += get_dimension(DIM_DOCUMENT_MARGIN) - page->scroll_position;

                        if ((r.y2 >= 0) || (r.y < window_height))
                        {
                            set_color((i == page->search_index) ? COLOR_INDEX_SEARCH_SELECTED : COLOR_INDEX_SEARCHES);
                            int border = 1;
                            draw_rectangle(r.x - border, r.y - border,
                                    r.x2 - r.x + 2 * border, r.y2 - r.y + 2 * border);
                        }
                    }
                }

                /* draw link hovering */
                {
                    int link_number = sb_count(page->links);
                    for (int i = 0; i < link_number; i++)
                    {
                        recti r = page->links[i].document_rectangle;

                        r.x += get_dimension(DIM_DOCUMENT_MARGIN);
                        r.x2 += get_dimension(DIM_DOCUMENT_MARGIN);
                        r.y += get_dimension(DIM_DOCUMENT_MARGIN) - page->scroll_position;
                        r.y2 += get_dimension(DIM_DOCUMENT_MARGIN) - page->scroll_position;

                        if ((r.y2 >= 0) || (r.y < window_height))
                        {
                            if (page->links[i].highlight)
                            {
                                set_color(COLOR_INDEX_LINK);
                                int link_border = 1;
                                draw_rectangle(r.x - link_border, r.y - link_border,
                                        r.x2 - r.x + 2 * link_border, r.y2 - r.y + 2 * link_border);
                            }
                        }
                    }
                }

                render_manpage(page);

                /* draw the search input if active */
                if (page->search_input_active)
                {
                    int input_height = get_line_height() * 3 / 2;
                    int input_width = get_character_width() * 30;
                    set_color(COLOR_INDEX_BACKGROUND);
                    draw_rectangle(0, window_height - input_height, input_width, input_height);
                    set_color(COLOR_INDEX_GUI_1);
                    draw_rectangle_outline(0, window_height - input_height, input_width, input_height);

                    if (strlen(page->search_string) == 0)
                    {
                        set_color(COLOR_INDEX_DIM);
                        draw_string("Search", get_dimension(DIM_TEXT_HORIZONTAL_MARGIN), window_height - input_height + get_dimension(DIM_TEXT_HORIZONTAL_MARGIN));
                    }
                    else
                    {
                        set_color((page->search_num > 0) ? COLOR_INDEX_FOREGROUND : COLOR_INDEX_ERROR);
                        draw_string(page->search_string, get_dimension(DIM_TEXT_HORIZONTAL_MARGIN), window_height - input_height + get_dimension(DIM_TEXT_HORIZONTAL_MARGIN));
                    }
                }

                /* draw the scrollbar */
                set_color(COLOR_INDEX_SCROLLBAR_BACKGROUND);
                draw_rectangle(window_width - get_dimension(DIM_SCROLLBAR_WIDTH), 0, get_dimension(DIM_SCROLLBAR_WIDTH), window_height);

                update_scrollbar();

                if (scrollbar_thumb_hover)
                {
                    set_color(COLOR_INDEX_SCROLLBAR_THUMB_HOVER);
                }
                else
                {
                    set_color(COLOR_INDEX_SCROLLBAR_THUMB);
                }

                draw_rectangle(window_width - get_dimension(DIM_SCROLLBAR_WIDTH) + get_dimension(DIM_SCROLLBAR_THUMB_MARGIN), scrollbar_thumb_position,
                        get_dimension(DIM_SCROLLBAR_WIDTH) - 1 * get_dimension(DIM_SCROLLBAR_THUMB_MARGIN), scrollbar_thumb_size);
            }
            break;

        case D_SEARCH:
        default:
            {
                set_color(COLOR_INDEX_GUI_1);
                int input_height = get_line_height() * 3 / 2;

                int top = 100;
                int top_result_box = top + input_height + get_dimension(DIM_GUI_PADDING);
                int text_vertical_offset = ceil(0.5 * (input_height - get_line_height()));

                draw_rectangle_outline(window_width / 2 - get_dimension(DIM_SEARCH_WIDTH) / 2, top,
                        get_dimension(DIM_SEARCH_WIDTH), input_height);

                set_color(COLOR_INDEX_SCROLLBAR_BACKGROUND);
                draw_rectangle_outline(window_width / 2 - get_dimension(DIM_SEARCH_WIDTH) / 2, top_result_box,
                        get_dimension(DIM_SEARCH_WIDTH), results_shown_lines * input_height);

                set_color(COLOR_INDEX_FOREGROUND);
                const char *text = "Type to search...";
                if (strlen(search_term) != 0)
                {
                    text = search_term;
                }

                draw_string(text, window_width / 2 - get_dimension(DIM_SEARCH_WIDTH) / 2 + get_dimension(DIM_TEXT_HORIZONTAL_MARGIN), top + text_vertical_offset);

                /* draw search results */
                for (int i = 0; i < results_shown_lines; i++)
                {
                    int real_index = i + results_view_offset;

                    if (real_index < matches_count)
                    {
                        draw_string(manpage_names[matches[real_index].idx],
                                window_width / 2 - get_dimension(DIM_SEARCH_WIDTH) / 2 + get_dimension(DIM_TEXT_HORIZONTAL_MARGIN), top_result_box + i * input_height + text_vertical_offset);
                    }
                }

                if ((results_selected_index >= 0) && (results_selected_index < matches_count))
                {
                    set_color(COLOR_INDEX_GUI_2);
                    int index_on_view = results_selected_index - results_view_offset;
                    draw_rectangle_outline(window_width / 2 - get_dimension(DIM_SEARCH_WIDTH) / 2, top_result_box + index_on_view * input_height,
                            get_dimension(DIM_SEARCH_WIDTH), input_height);
                }

                {
                    char tmp[128];
                    if (matches_count == 1)
                    {
                        sprintf(tmp, "1 match");
                    }
                    else
                    {
                        sprintf(tmp, "%d matches", matches_count);
                    }

                    set_color(COLOR_INDEX_DIM);
                    draw_string(tmp, window_width / 2 - strlen(tmp) * get_character_width() / 2, top_result_box + results_shown_lines * input_height + text_vertical_offset);
                }
            }
            break;
    }


#if 0
    if (loadedFont && (loadedFont->texture_id > 0))
    {
        set_color(COLOR_INDEX_FOREGROUND);
        glBindTexture(GL_TEXTURE_2D, loadedFont->texture_id);

        glEnable(GL_BLEND);
        glEnable(GL_TEXTURE_2D);

        int x =10;
        int y= 10;
        int w = loadedFont->bitmap_width;
        int h = loadedFont->bitmap_height;
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x, y);
        glTexCoord2f(0, 1); glVertex2f(x, y + h);
        glTexCoord2f(1, 1); glVertex2f(x + w, y + h);
        glTexCoord2f(1, 0); glVertex2f(x + w, y);
        glEnd();
    }
#endif

    glutSwapBuffers();
}

int clamp_scroll_position(int new_scroll_position)
{
    int doc_height = document_height();
    return clamp(new_scroll_position, 0, (doc_height - window_height) > 0 ? doc_height - window_height : 0);
}

void set_scroll_position(int new_scroll_position)
{
    new_scroll_position = clamp_scroll_position(new_scroll_position);

    if (new_scroll_position != page->scroll_position)
    {
        page->scroll_position = new_scroll_position;
        glutPostRedisplay();
    }
}

recti to_document_coordinates(recti r)
{
    r.x += get_dimension(DIM_DOCUMENT_MARGIN);
    r.x2 += get_dimension(DIM_DOCUMENT_MARGIN);

    r.y += get_dimension(DIM_DOCUMENT_MARGIN);
    r.y2 += get_dimension(DIM_DOCUMENT_MARGIN);

    return r;
}

void scroll_in_view(recti r, int prefered_scroll_position)
{
    int scroll_offset = 3 * get_line_advance();

    if ((r.y - scroll_offset) < prefered_scroll_position)
    {
        page->scroll_position = clamp_scroll_position(r.y - scroll_offset);
    }
    else if ((r.y2 + scroll_offset) > (prefered_scroll_position + window_height))
    {
        page->scroll_position = clamp_scroll_position(r.y2 - window_height + scroll_offset);
    }
    else
    {
        page->scroll_position = prefered_scroll_position;
    }
}

int scrollbar_thumb_hittest(int x, int y)
{
    if ((x > (window_width - get_dimension(DIM_SCROLLBAR_WIDTH))) && (y >= scrollbar_thumb_position) && (y < scrollbar_thumb_position + scrollbar_thumb_size))
        return 1;
    else
        return 0;
}

int results_hittest(int x, int y)
{
    int input_height = get_line_height() * 3 / 2;

    int top = 100;
    int top_result_box = top + input_height + get_dimension(DIM_GUI_PADDING);

    for (int i = 0; i < results_shown_lines; i++)
    {
        if ((x >= window_width / 2 - get_dimension(DIM_SEARCH_WIDTH) / 2) &&
                (x < window_width / 2 - get_dimension(DIM_SEARCH_WIDTH) / 2 + get_dimension(DIM_SEARCH_WIDTH)) &&
                (y >= top_result_box + i * input_height) &&
                (y < top_result_box + i * input_height + input_height))
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

        r.x += get_dimension(DIM_DOCUMENT_MARGIN);
        r.x2 += get_dimension(DIM_DOCUMENT_MARGIN);
        r.y += get_dimension(DIM_DOCUMENT_MARGIN) - page->scroll_position;
        r.y2 += get_dimension(DIM_DOCUMENT_MARGIN) - page->scroll_position;

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
                        else if (x >= (window_width - get_dimension(DIM_SCROLLBAR_WIDTH)))
                        {
                            // page up or down if clicked outside the thumb
                            if (y < scrollbar_thumb_position)
                            {
                                set_scroll_position(page->scroll_position - (window_height - get_line_advance()));
                            }
                            else if (y >= (scrollbar_thumb_position + scrollbar_thumb_size))
                            {
                                set_scroll_position(page->scroll_position + window_height - get_line_advance());
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
                        set_scroll_position(page->scroll_position - get_dimension(DIM_SCROLL_AMOUNT));
                    }
                    break;
                case 4:
                    if (state == GLUT_DOWN)
                    {
                        set_scroll_position(page->scroll_position + get_dimension(DIM_SCROLL_AMOUNT));
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

                    r.x += get_dimension(DIM_DOCUMENT_MARGIN);
                    r.x2 += get_dimension(DIM_DOCUMENT_MARGIN);
                    r.y += get_dimension(DIM_DOCUMENT_MARGIN) - page->scroll_position;
                    r.y2 += get_dimension(DIM_DOCUMENT_MARGIN) - page->scroll_position;

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
        if (page->search_input_active)
        {
            switch (key)
            {
                case 3: /* ctrl-c */
                case 4: /* ctrl-d */
                case 27: /* escape */
                    page->search_input_active = 0;
                    set_scroll_position(page->search_start_scroll_position);
                    glutPostRedisplay();
                    break;
                case '\n':
                case '\r':
                    /* save the search */
                    page->search_input_active = 0;
                    glutPostRedisplay();
                    break;
                case '\b':
                    {
                        int len = strlen(page->search_string);
                        if (len > 0)
                        {
                            page->search_string[len - 1] = 0;
                            update_page_search(page);
                            if (page->search_num > 0)
                            {
                                scroll_in_view(to_document_coordinates(page->searches[page->search_index].document_rectangle), page->search_start_scroll_position);
                            }
                            glutPostRedisplay();
                        }
                    }
                    break;
                default:
                    if (strlen(page->search_string) <= (ARRAY_SIZE(page->search_string) - 2))
                    {
                        strcat(page->search_string, (char[]){key, 0});
                        update_page_search(page);
                        if (page->search_num > 0)
                        {
                            scroll_in_view(to_document_coordinates(page->searches[page->search_index].document_rectangle), page->search_start_scroll_position);
                        }
                        glutPostRedisplay();
                    }
                    break;
            }

            return;
        }

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
            case 'q':
            case 'Q':
                exit(EXIT_SUCCESS);
            case 'b':
            case 27: /* escape */
                page_back();
                break;
            case 6: /* ctrl-f */
                display_mode = D_SEARCH;
                glutPostRedisplay();
                break;
            case '/':
                page->search_string[0] = 0;
                page->search_num = 0;
                page->search_index = 0;
                page->search_start_scroll_position = page->scroll_position;
                page->search_visible = 1;
                page->search_input_active = 1;
                glutPostRedisplay();
                break;
            case '\n':
            case '\r':
                /* clear search */
                page->search_num = 0;
                page->search_index = 0;
                page->search_string[0] = 0;
                page->search_visible = 0;
                glutPostRedisplay();
                break;
            case 'n':
                if (page->search_visible)
                {
                    page->search_index++;
                    if (page->search_index >= page->search_num)
                        page->search_index -= page->search_num;

                    scroll_in_view(to_document_coordinates(page->searches[page->search_index].document_rectangle), page->scroll_position);

                    glutPostRedisplay();
                }
                break;
            case 'N':
                if (page->search_visible)
                {
                    page->search_index--;
                    if (page->search_index < 0)
                        page->search_index += page->search_num;

                    scroll_in_view(to_document_coordinates(page->searches[page->search_index].document_rectangle), page->scroll_position);

                    glutPostRedisplay();
                }
                break;
            case 'f':
                page_forward();
                break;
            case 'i':
                glutReshapeWindow(fitting_window_width(), window_height);
                break;
            case 'o':
                glutReshapeWindow(fitting_window_width(), window_height);
                break;
            case 'k':
                set_scroll_position(page->scroll_position - get_dimension(DIM_SCROLL_AMOUNT));
                break;
            case 'j':
                set_scroll_position(page->scroll_position + get_dimension(DIM_SCROLL_AMOUNT));
                break;
            case 'K':
                set_scroll_position(page->scroll_position - 5 * get_dimension(DIM_SCROLL_AMOUNT));
                break;
            case 'J':
                set_scroll_position(page->scroll_position + 5 * get_dimension(DIM_SCROLL_AMOUNT));
                break;
            case 'G':
                set_scroll_position(1000000000);
                break;
            case ' ':
                {
                    int mod = glutGetModifiers();

                    if (mod & GLUT_ACTIVE_SHIFT)
                        set_scroll_position(page->scroll_position - (window_height - get_line_advance()));
                    else
                        set_scroll_position(page->scroll_position + window_height - get_line_advance());
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
            case 27: /* escape */
                {
                    int len = strlen(search_term);
                    if (len > 0)
                    {
                        search_term[0] = 0;
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
                    set_scroll_position(page->scroll_position - get_dimension(DIM_SCROLL_AMOUNT));
                    break;
                case GLUT_KEY_DOWN:
                    set_scroll_position(page->scroll_position + get_dimension(DIM_SCROLL_AMOUNT));
                    break;
                case GLUT_KEY_PAGE_UP:
                    set_scroll_position(page->scroll_position - (window_height - get_line_advance()));
                    break;
                case GLUT_KEY_PAGE_DOWN:
                    set_scroll_position(page->scroll_position + window_height - get_line_advance());
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

int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char **)a, *(char **)b);
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
                        char key[576];
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

    qsort(manpage_names, sb_count(manpage_names), sizeof(char *), &cmp_str);

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
    switch (display_mode)
    {
        case D_MANPAGE:
            {
                char window_title[576];

                if (strlen(page->manpage_name) > 0)
                {
                    sprintf(window_title, "%s(%s) - mangl",
                            page->manpage_name, page->manpage_section);
                }
                else
                {
                    sprintf(window_title, "%s - mangl", page->filename);
                }
                glutSetWindowTitle(window_title);
            }
            break;
        case D_SEARCH:
        default:
            glutSetWindowTitle("mangl");
            break;
    }
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
    else if (display_mode == D_MANPAGE)
    {
        display_mode = D_SEARCH;
        stack_pos = 0;
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

int parse_line(char *line, char *name_out, char *value_out)
{
    /* eat beginning whitespace */
    while ((*line && ((*line == ' ') || (*line == '\t'))))
        line++;

    if (*line == '\0')
        return -1;

    if (*line == '\n')
        return -1; /* empty line */

    if (*line == '#') /* comment */
        return -2;

    char *value = strchr(line, ':');
    if (value == NULL)
        return -3;

    *value = 0;
    value++;

    char *space_pos = strchr(line, ' ');
    if (space_pos != NULL)
    {
        *space_pos = 0;
    }

    while ((*value && ((*value == ' ') || (*value == '\t'))))
        value++;

    if (*value == '\0')
        return -1;

    if (*value == '\n')
        return -1; /* empty line */

    /* trim whitespace on the end of value */
    char *end = value + strlen(value) - 1;

    while (*end && ((*end == '\n') || (*end == ' ')))
    {
        *end = 0;
        end--;
    }

    if (name_out)
    {
        strcpy(name_out, line);
    }

    if (value_out)
    {
        strcpy(value_out, value);
    }

    return 0;
}

void parse_color(const char *value, float *rgb)
{
    if (value[0] == '#')
    {
        uint32_t color_value = 0;
        if (sscanf(value + 1, "%x", &color_value) == 1)
        {
            rgb[0] = (color_value >> 16) / 255.0f;
            rgb[1] = ((color_value >> 8) & 0xff) / 255.0f;
            rgb[2] = (color_value & 0xff) / 255.0f;
        }
    }
}

void load_settings(void)
{
    const char *rc_filename = ".manglrc";
    char *home = getenv("HOME");

    char settings_filename[256];
    snprintf(settings_filename, sizeof(settings_filename), "%s/%s", home, rc_filename);

    FILE *f = fopen(settings_filename, "rb");
    if (f == NULL)
    {
        return;
    }

    char line[1024];

    while (!feof(f))
    {
        line[0] = 0;
        if (fgets(line, sizeof(line), f) != NULL)
        {
            char name[256];
            char value[256];
            if (parse_line(line, name, value) == 0)
            {
                if (strcmp(name, "font") == 0)
                {
                    strcpy(settings.font_file, value);
                }
                else if (strcmp(name, "font_size") == 0)
                {
                    settings.font_size = atoi(value);
                }
                else if (strcmp(name, "gui_scale") == 0)
                {
                    char *end = NULL;
                    double val = strtod(value, &end);
                    if ((end == NULL) || (end == value))
                    {
                        fprintf(stderr, "Failed to read value: \"%s\" from config file.\n", value);
                    }
                    else
                    {
                        settings.gui_scale = val;
                    }
                }
                else if (strcmp(name, "line_spacing") == 0)
                {
                    char *end = NULL;
                    double val = strtod(value, &end);
                    if ((end == NULL) || (end == value))
                    {
                        fprintf(stderr, "Failed to read value: \"%s\" from config file.\n", value);
                    }
                    else
                    {
                        settings.line_spacing = val;
                    }
                }
                else if (strcmp(name, "initial_window_rows") == 0)
                {
                    initial_window_rows = atoi(value);
                }
                else if (strcmp(name, "color_background") == 0)
                    parse_color(value, color_table[COLOR_INDEX_BACKGROUND]);
                else if (strcmp(name, "color_foreground") == 0)
                    parse_color(value, color_table[COLOR_INDEX_FOREGROUND]);
                else if (strcmp(name, "color_bold") == 0)
                    parse_color(value, color_table[COLOR_INDEX_BOLD]);
                else if (strcmp(name, "color_italic") == 0)
                    parse_color(value, color_table[COLOR_INDEX_ITALIC]);
                else if (strcmp(name, "color_dim") == 0)
                    parse_color(value, color_table[COLOR_INDEX_DIM]);
                else if (strcmp(name, "color_link") == 0)
                    parse_color(value, color_table[COLOR_INDEX_LINK]);
                else if (strcmp(name, "color_scrollbar_background") == 0)
                    parse_color(value, color_table[COLOR_INDEX_SCROLLBAR_BACKGROUND]);
                else if (strcmp(name, "color_scrollbar_thumb") == 0)
                    parse_color(value, color_table[COLOR_INDEX_SCROLLBAR_THUMB]);
                else if (strcmp(name, "color_scrollbar_thumb_hover") == 0)
                    parse_color(value, color_table[COLOR_INDEX_SCROLLBAR_THUMB_HOVER]);
                else if (strcmp(name, "color_gui_1") == 0)
                    parse_color(value, color_table[COLOR_INDEX_GUI_1]);
                else if (strcmp(name, "color_gui_2") == 0)
                    parse_color(value, color_table[COLOR_INDEX_GUI_2]);
                else if (strcmp(name, "color_error") == 0)
                    parse_color(value, color_table[COLOR_INDEX_ERROR]);
            }
        }
    }

    fclose(f);
}

int main(int argc, char *argv[])
{
    char window_title[576];
    manpage_database = hashmap_new();

    load_settings();
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

        if (strcmp(argv[1], "--version") == 0)
        {
            printf("mangl %d.%d\n", MANGL_VERSION_MAJOR, MANGL_VERSION_MINOR);
            exit(EXIT_SUCCESS);
        }
        else if ((strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "--help") == 0))
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

    init_builtin_font();
    init_freetype();
    if (strlen(settings.font_file) > 0)
    {
        if (get_font_file(settings.font_file))
        {
            render_font_texture(settings.font_file, (int)(settings.gui_scale * settings.font_size));
            mainFont = loadedFont;
        }
        else
        {
            fprintf(stderr, "Can't find or resolve font file/name: \"%s\"\n", settings.font_file);
        }
    }

    /* display gui */
    if (fork() != 0)
    {
        exit(EXIT_SUCCESS);
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(0);
    glutInitWindowSize(fitting_window_width(), fitting_window_height(initial_window_rows));

    glutCreateWindow(window_title);

    glutReshapeFunc(&reshape);
    glutDisplayFunc(&render);

    glutMouseFunc(&mouse_func);
    glutMotionFunc(&mouse_motion_func);
    glutPassiveMotionFunc(&mouse_passive_motion_func);
    glutKeyboardFunc(&keyboard_func);
    glutSpecialFunc(&special_func);

    upload_font_textures();

    glutMainLoop();

    if (page)
        free_manpage(page);

    return 0;
}

