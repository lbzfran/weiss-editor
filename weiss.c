
/*** include ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

/*** constants ***/
#define WEISS_VERSION "0.1.0"
#define WEISS_TAB_AS_SPACES 1
#define WEISS_TAB_STOP 4
#define WEISS_QUIT_CONFIRM_COUNTER 1
#define WEISS_BACKSPACE_APPEND 1
#define WEISS_DISPLAY_DIRT_COUNTER 1
#define WEISS_SCROLL_Y_MARGIN 7

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** global ***/

struct editorSyntax {
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} erow;

struct editorConfig {
    int cx, cy;
    int rx;
    int rowoff;
    int coloff;
    int screenRows;
    int screenCols;
    int numRows;
    erow *row;
    int dirty;
    int mode;
    char *filename;
    char statusMsg[80];
    time_t statusMsgTime;
    struct editorSyntax *syntax;
    struct termios orig_termios;
};

struct editorConfig E;

/*** filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", NULL };
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};

// NOTE(liam): highlight database
struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** protos ***/

void editorSetStatusMessage(const char *fmt, ...);
int editorReadKey(void);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** term settings ***/


void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    write(STDIN_FILENO, "\033[2J\033[H\033[?1049l", 15);

    perror(s);
    exit(1);
}

void disableRawMode()
{
    write(STDIN_FILENO, "\033[2J\033[H\033[?1049l", 15);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    {
        die("tcsetattr");
    }
}

void enableRawMode()
{
    /*
     * Turns off several terminal bindings (C-c, C-v, C-d, C-m, etc.),
     * turns off terminal echo, and other misc settings.
     */
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    {
        die("tcsetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    {
        die("tcsetattr");
    }

    // NOTE(liam): turns on alternate screen buffer.
    write(STDIN_FILENO, "\033[?1049h\033[2J\033[H", 15);
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    {
        return -1;
    }

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
        {
            break;
        }
        if (buf[i] == 'R')
        {
            break;
        }
        i++;
    }

    if (buf[0] != '\x1b' || buf[1] != '[')
    {
        return -1;
    }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
        {
            return getCursorPosition(rows, cols);
        }
        editorReadKey();
        return -1;
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

int is_separator(int c)
{
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row)
{
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) { return; }

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = scs ? strlen(mcs) : 0;
    int mce_len = scs ? strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->rsize)
    {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment)
        {
            if (!strncmp(&row->render[i], scs, scs_len))
            {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string)
        {
            if (in_comment)
            {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len))
                {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                }
                else
                {
                    i++;
                    continue;
                }
            }
            else if (!strncmp(&row->render[i], mcs, mcs_len))
            {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS)
        {
            if (in_string)
            {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize)
                {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) { in_string = 0; }
                i++;
                prev_sep = 1;
                continue;
            }
            else
            {
                if (c == '"' || c == '\'')
                {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS)
        {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER))
            {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep)
        {
            int j;
            for (j = 0; keywords[j]; j++)
            {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;

                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen]))
                {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }

                if (keywords[j] != NULL)
                {
                    prev_sep = 0;
                    continue;
                }
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numRows)
    {
        editorUpdateSyntax(&E.row[row->idx + 1]);
    }
}

int editorSyntaxToColor(int hl)
{
    switch (hl)
    {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36;
        case HL_KEYWORD1: return 33;
        case HL_KEYWORD2: return 32;
        case HL_STRING: return 35;
        case HL_NUMBER: return 31;
        case HL_MATCH: return 7;
        default: return 37;
    }
}

void editorSelectSyntaxHighlight()
{
    E.syntax = NULL;
    if (E.filename == NULL) { return; }

    char *ext = strrchr(E.filename, '.');

    if (!ext || ext == E.filename) { return; }

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++)
    {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i])
        {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i])))
            {
                E.syntax = s;

                int filerow;
                for (filerow = 0; filerow < E.numRows; filerow++)
                {
                    editorUpdateSyntax(&E.row[filerow]);
                }

                return;
            }
            i++;
        }
    }
}

/*** row ops ***/

int editorRowCxToRx(erow *row, int cx)
{
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
        {
            rx += (WEISS_TAB_STOP - 1) - (rx % WEISS_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;

    for (cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
        {
            cur_rx += (WEISS_TAB_STOP - 1) - (cur_rx % WEISS_TAB_STOP);
        }
        cur_rx++;

        if (cur_rx > rx) { return cx; }
    }
    return cx;
}

void editorUpdateRow(erow *row)
{
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t') { tabs++; }
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (WEISS_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = '%';
            while (idx % WEISS_TAB_STOP != 0) { row->render[idx++] = ' '; }
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numRows) { return; }

    E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numRows - at));
    for (int j = at + 1; j <= E.numRows; j++) { E.row[j].idx++; }

    E.row[at].idx = at;

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numRows++;
    E.dirty++;
}

void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at)
{
    if (at < 0 || at >= E.numRows) { return; }
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numRows - at - 1));
    for (int j = at; j < E.numRows - 1; j++) { E.row[j].idx--; }
    E.numRows--;
    /*E.dirty++;*/
}

void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size) { at = row->size; }
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at >= row->size) { return; }
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor ops ***/

void editorInsertChar(int c)
{
    if (E.cy == E.numRows)
    {
        editorInsertRow(E.numRows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline()
{
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    }
    else
    {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar()
{
    if (E.cy == E.numRows) { return; }
    if (E.cx == 0 && E.cy == 0) { return; }

    erow *row = &E.row[E.cy];
    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else if (WEISS_BACKSPACE_APPEND)// NOTE(liam): implicitly E.cx == 0
    {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

int getScreenCenter(void)
{
    int center = E.cy - E.screenRows / 2;
    if (center < 0)
    {
        center = 0;
    }
    else if (center >= E.numRows - E.screenRows)
    {
        center = E.numRows - E.screenRows;
    }
    return center;
}

/*** file i/o ***/

char *editorRowsToString(int *buflen)
{
    int totlen = 0;
    int j;
    for (j = 0; j < E.numRows; j++)
    {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numRows; j++)
    {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename)
{
    free(E.filename);
    E.filename = strdup(filename);


    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    editorSelectSyntaxHighlight();

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                    line[linelen - 1] == '\r'))
        { linelen--; }
        editorInsertRow(E.numRows, line, linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave()
{
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s", NULL);
        if (E.filename == NULL)
        {
            editorSetStatusMessage("Save cancelled");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1)
    {

        if (ftruncate(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key)
{
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl)
    {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b')
    {
        last_match = -1;
        direction = 1;
        return;
    }
    else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    {
        direction = 1;
    }
    else if (key == ARROW_LEFT || key == ARROW_UP)
    {
        direction = -1;
    }
    else
    {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) { direction = 1; }
    int current = last_match;
    int i;
    for (i = 0; i < E.numRows; i++)
    {
        current += direction;
        if (current == -1) { current = E.numRows - 1; }
        else if (current == E.numRows) { current = 0; }

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match)
        {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);

            E.rowoff = getScreenCenter();

            int match_len = strlen(query);
            memset(&row->hl[match - row->render], HL_MATCH, match_len);
            break;
        }
    }
}

void editorFind()
{
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (ESC/Arrows/Enter)", editorFindCallback);
    if (query)
    {
        free(query);
    }
    else
    {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** undo/redo ***/

// TODO(liam): make these
void editorUndo(void);
void editorRedo(void);


int editorReadKey()
{
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN)
        {
            die("read");
        }
    }

    if (c == '\x1b')
    {
        char seq[3] = {0};

        if (read(STDIN_FILENO, &seq[0], 1) != 1) { return '\x1b'; }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) { return '\x1b'; }

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) { return '\x1b'; }

                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch(seq[1])
            {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }
    else
    {
        return c;
    }
}

/*** append buf ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
    {
        return;
    }
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** output ***/

void editorScroll()
{
    E.rx = 0;
    if (E.cy < E.numRows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenRows)
    {
        E.rowoff = E.cy - E.screenRows + 1;
    }
    if (E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screenCols)
    {
        E.coloff = E.rx - E.screenCols + 1;
    }
}

void editorIndentDown() {
    if (E.cy >= E.numRows) return;
    erow *row = &E.row[E.cy];
    if (row->size == 0) return;  // Nothing to unindent.

    int removeCount = 0;
    if (WEISS_TAB_AS_SPACES) {
        // Count up to WEISS_TAB_STOP leading spaces.
        for (int i = 0; i < WEISS_TAB_STOP && i < row->size; i++) {
            if (row->chars[i] == ' ') {
                removeCount++;
            } else {
                break;
            }
        }
    } else {
        // If not using spaces, remove a tab if present.
        if (row->chars[0] == '\t')
            removeCount = 1;
    }
    if (removeCount == 0) return;  // No indent found.

    // Remove the indent by shifting the rest of the row left.
    memmove(row->chars, row->chars + removeCount, row->size - removeCount + 1); // include null terminator
    row->size -= removeCount;
    editorUpdateRow(row);

    // Adjust the cursor: reduce E.cx by removeCount, ensuring it doesn't go negative.
    if (E.cx >= removeCount) {
        E.cx -= removeCount;
    } else {
        E.cx = 0;
    }
    E.dirty++;
}

void editorIndentUp() {
    if (E.cy >= E.numRows) return;
    erow *row = &E.row[E.cy];

    // Determine indent string and length.
    int indentSize = WEISS_TAB_STOP;  // Number of spaces if using spaces.
    char indentStr[WEISS_TAB_STOP + 1];
    if (WEISS_TAB_AS_SPACES) {
        memset(indentStr, ' ', indentSize);
    } else {
        // If not using spaces, just insert a tab.
        indentStr[0] = '\t';
        indentSize = 1;
    }
    indentStr[indentSize] = '\0';

    // Reallocate to make room for the indent.
    row->chars = realloc(row->chars, row->size + indentSize + 1);
    // Shift existing characters to the right.
    memmove(row->chars + indentSize, row->chars, row->size + 1); // include null terminator
    // Copy the indent string into the beginning.
    memcpy(row->chars, indentStr, indentSize);
    row->size += indentSize;
    editorUpdateRow(row);

    // Adjust the cursor: if not at the beginning, shift it right.
    E.cx += indentSize;
    E.dirty++;
}

void editorRowAppendToPrev()
{
    if (E.cy == 0) { return; }

    erow *currentRow = &E.row[E.cy];
    erow *prevRow = &E.row[E.cy - 1];

    int indent = 0;
    while (indent < currentRow->size &&
            (currentRow->chars[indent] == ' ' ||
             currentRow->chars[indent] == '\t'))
    { indent++; }


    editorRowAppendString(prevRow, currentRow->chars + indent, currentRow->size - indent);

    editorDelRow(E.cy);

    E.cy--;
    E.cx = prevRow->size;
}

void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenRows; y++)
    {
        int filerow = y + E.rowoff;
        if (filerow >= E.numRows) {
            if (E.numRows == 0 && y == E.screenRows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                        "weiss editor -- version %s", WEISS_VERSION);
                if (welcomelen > E.screenCols)
                {
                    welcomelen = E.screenCols;
                }

                int padding = (E.screenCols - welcomelen) / 2;
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                /*write(STDOUT_FILENO, "~", 1);*/
                abAppend(ab, "~", 1);
            }
        }
        else
        {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) { len = 0; }
            if (len > E.screenCols) { len = E.screenCols; }

            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            int j;

            for (j = 0; j < len; j++)
            {
                if (iscntrl(c[j]))
                {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1)
                    {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                }
                else if (hl[j] == HL_NORMAL)
                {
                    if (current_color != -1)
                    {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                }
                else
                {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color)
                    {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                    if (hl[j] == HL_MATCH)
                    {
                        // NOTE(liam): removes highlighting
                        abAppend(ab, "\x1b[m", 3);
                    }
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }

        // NOTE(liam): Erase inline.
        abAppend(ab, "\x1b[K", 3);
        /*write(STDOUT_FILENO, "\r\n", 2);*/
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80], dirtstatus[6];

    int dirtlen = snprintf(dirtstatus, sizeof(dirtstatus), "[%d]", E.dirty < 999 ? E.dirty : 999);

    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[.]", E.numRows,
                       WEISS_DISPLAY_DIRT_COUNTER ? (E.dirty && dirtlen ? dirtstatus : "") :
                       (E.dirty ? "[+]" : ""));
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d | %s",
                        E.cy + 1, E.cx + 1,
                        E.syntax ? E.syntax->filetype : "nil");
    if (len > E.screenCols) { len = E.screenCols; }
    abAppend(ab, status, len);
    while (len < E.screenCols)
    {
        if (E.screenCols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusMsg);
    if (msglen > E.screenCols) { msglen = E.screenCols; }
    if (msglen && time(NULL) - E.statusMsgTime < 5)
    {
        abAppend(ab, E.statusMsg, msglen);
    }
}

void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    /*write(STDOUT_FILENO, "\x1b[2J", 4);*/
    /*write(STDOUT_FILENO, "\x1b[H", 3);*/
    abAppend(&ab, "\x1b[?25l", 6);
    /*abAppend(&ab, "\x1b[2J", 4);*/
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                              (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusMsg, sizeof(E.statusMsg), fmt, ap);
    va_end(ap);
    E.statusMsgTime = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0) { buf[--buflen] = '\0'; }
        }
        else if (c == '\x1b')
        {
            editorSetStatusMessage("");
            if (callback) { callback(buf, c); }
            free(buf);
            return NULL;
        }
        else if (c == '\r')
        {
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                if (callback) { callback(buf, c); }
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) { callback(buf, c); }
    }
}

void editorMoveCursor(int key)
{
    erow *row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];

    switch (key)
    {
        case ARROW_LEFT:
        {
            if (E.cx != 0)
            {
                E.cx--;
            }
            else if (E.cy > 0)
            {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
        } break;
        case ARROW_RIGHT:
        {
            if (row && E.cx < row->size)
            {
                E.cx++;
            }
            else if (row && E.cx == row->size)
            {
                E.cy++;
                E.cx = 0;
            }
        } break;
        case ARROW_UP:
        {
            if (E.cy != 0)
            {
                E.cy--;
            }
        } break;
        case ARROW_DOWN:
        {
            if (E.cy < E.numRows)
            {
                E.cy++;
            }
        } break;
    }

    row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) { E.cx = rowlen; }

    int margin = WEISS_SCROLL_Y_MARGIN;

    if (E.cy < E.rowoff + margin)
    {
        E.rowoff = E.cy - margin;
        if (E.rowoff < 0) { E.rowoff = 0; }
    }
    else if (E.cy >= E.rowoff + E.screenRows - margin)
    {
        E.rowoff = E.cy - E.screenRows + margin + 1;
    }
}

void editorProcessKeypress()
{
    static int quitTimes = WEISS_QUIT_CONFIRM_COUNTER;
    int c = editorReadKey();

    switch (c)
    {
        case '\r':
        {
            editorInsertNewline();
        } break;
        case CTRL_KEY('q'):
        {
            if (E.dirty && quitTimes > 0)
            {
                editorSetStatusMessage("UNSAVED CHANGES: "
                    "Press C-Q %d more times to quit.", quitTimes);
                quitTimes--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
        } break;

        case CTRL_KEY('s'):
        {
            editorSave();
        } break;

        case CTRL_KEY('l'):
        {
            E.rowoff = getScreenCenter();
        } break;

        case CTRL_KEY('z'):
        {
            editorSetStatusMessage("undo not implemented");
        } break;

        case CTRL_KEY('j'):
        {
            editorRowAppendToPrev();
        } break;
        case HOME_KEY:
        {
            E.cx = 0;
        } break;
        case END_KEY:
        {
            if (E.cy < E.numRows)
            {
                E.cx = E.row[E.cy].size;
            }
        } break;

        case CTRL_KEY('n'):
        {
            editorIndentDown();
        } break;
        case CTRL_KEY('p'):
        {
            editorIndentUp();
        } break;

        case CTRL_KEY('f'):
        {
            editorFind();
        } break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
        {
            if (c == DEL_KEY) { editorMoveCursor(ARROW_RIGHT); }
            editorDelChar();
        } break;

        case PAGE_UP:
        case PAGE_DOWN:
        {
            if (c == PAGE_UP)
            {
                E.cy = E.rowoff;
            }
            else if (c == PAGE_DOWN)
            {
                E.cy = E.rowoff + E.screenRows - 1;
                if (E.cy > E.numRows) { E.cy = E.numRows; }
            }

            int times = E.screenRows;
            while (times--)
            {
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        } break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
        {
            editorMoveCursor(c);
        } break;

        /*case CTRL_KEY('l'):*/
        case '\x1b':
        {
        } break;

        case '\t':
        {
            if (WEISS_TAB_AS_SPACES)
            {
                for (unsigned int i = 0; i < WEISS_TAB_STOP; i++)
                {
                    editorInsertChar(' ');
                }
                break;
            }
            __attribute__ ((fallthrough));
        }
        default:
        {
            editorInsertChar(c);
        } break;
    }

    quitTimes = WEISS_QUIT_CONFIRM_COUNTER;
}

void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numRows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.mode = 0;
    E.filename = NULL;
    E.statusMsg[0] = '\0';
    E.statusMsgTime = 0;
    E.syntax = NULL;

    if (getWindowSize(&E.screenRows, &E.screenCols) == -1)
    {
        die("getWindowSize");
    }
    E.screenRows -= 2;
}

int main(int argc, char **argv)
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: C-S = save | C-Q = quit | C-F = find");
    editorRefreshScreen();
    while (1)
    {
        editorProcessKeypress();
        editorRefreshScreen();
    }
    return 0;
}
