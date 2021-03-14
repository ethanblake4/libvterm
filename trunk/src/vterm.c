#include "vterm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>

/* suck up the non-standard openpty/forkpty */
#if defined(__FreeBSD__)
# include <sys/ioctl.h>
# include <libutil.h>
# include <termios.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
# include <sys/ioctl.h>
# include <termios.h>
# include <util.h>
#else
# include <pty.h>
#endif

#define DEBUG_PRINT_INPUT 0

/*****************
 * API functions *
 *****************/

static void *default_malloc(size_t size, void *allocdata)
{
  void *ptr = malloc(size);
  if(ptr)
    memset(ptr, 0, size);
  return ptr;
}

static void default_free(void *ptr, void *allocdata)
{
  free(ptr);
}

static VTermAllocatorFunctions default_allocator = {
  .malloc = &default_malloc,
  .free   = &default_free,
};

VTerm *vterm_new(int rows, int cols)
{
  return vterm_new_with_allocator(rows, cols, &default_allocator, NULL);
}

VTerm *vterm_new_with_allocator(int rows, int cols, VTermAllocatorFunctions *funcs, void *allocdata)
{
  /* Need to bootstrap using the allocator function directly */
  VTerm *vt = (*funcs->malloc)(sizeof(VTerm), allocdata);

  vt->allocator = funcs;
  vt->allocdata = allocdata;

  vt->rows = rows;
  vt->cols = cols;

  vt->parser.state = NORMAL;

  vt->parser.callbacks = NULL;
  vt->parser.cbdata    = NULL;

  vt->parser.strbuffer_len = 64;
  vt->parser.strbuffer_cur = 0;
  vt->parser.strbuffer = vterm_allocator_malloc(vt, vt->parser.strbuffer_len);

  vt->outfunc = NULL;
  vt->outdata = NULL;

  vt->outbuffer_len = 64;
  vt->outbuffer_cur = 0;
  vt->outbuffer = vterm_allocator_malloc(vt, vt->outbuffer_len);

  vt->tmpbuffer_len = 64;
  vt->tmpbuffer = vterm_allocator_malloc(vt, vt->tmpbuffer_len);

  vt->g_out = g_string_sized_new(256);

  return vt;
}

void vterm_free(VTerm *vt)
{
  if(vt->screen)
    vterm_screen_free(vt->screen);

  if(vt->state)
    vterm_state_free(vt->state);

  vterm_allocator_free(vt, vt->parser.strbuffer);
  vterm_allocator_free(vt, vt->outbuffer);

  vterm_allocator_free(vt, vt);
}

INTERNAL void *vterm_allocator_malloc(VTerm *vt, size_t size)
{
  return (*vt->allocator->malloc)(size, vt->allocdata);
}

INTERNAL void vterm_allocator_free(VTerm *vt, void *ptr)
{
  (*vt->allocator->free)(ptr, vt->allocdata);
}

void vterm_get_size(const VTerm *vt, int *rowsp, int *colsp)
{
  if(rowsp)
    *rowsp = vt->rows;
  if(colsp)
    *colsp = vt->cols;
}

void vterm_set_size(VTerm *vt, int rows, int cols)
{
  vt->rows = rows;
  vt->cols = cols;

  if(vt->parser.callbacks && vt->parser.callbacks->resize)
    (*vt->parser.callbacks->resize)(rows, cols, vt->parser.cbdata);
}

int vterm_get_utf8(const VTerm *vt)
{
  return vt->mode.utf8;
}

void vterm_set_utf8(VTerm *vt, int is_utf8)
{
  vt->mode.utf8 = is_utf8;
}

static void term_output(const char *s, size_t len, void *user)
{
    VTerm *vt = user;

    g_string_append_len(vt->g_out, s, len);
}

void vterm_output_set_callback(VTerm *vt, VTermOutputCallback *func, void *user)
{
  //vt->outfunc = func;
  //vt->outdata = user;
  vt->outfunc = term_output;
  vt->outdata = vt;
}

INTERNAL void vterm_push_output_bytes(VTerm *vt, const char *bytes, size_t len)
{
    DEBUG_LOG("libvterm: pushoutputbytes\n");
  if(vt->outfunc) {
      DEBUG_LOG("libvterm: outfunc\n");
      DEBUG_LOG(bytes);
    (vt->outfunc)(bytes, len, vt->outdata);
    return;
  }

  if(len > vt->outbuffer_len - vt->outbuffer_cur)
    return;

  memcpy(vt->outbuffer + vt->outbuffer_cur, bytes, len);
  vt->outbuffer_cur += len;
}

INTERNAL void vterm_push_output_vsprintf(VTerm *vt, const char *format, va_list args)
{
  size_t len = vsnprintf(vt->tmpbuffer, vt->tmpbuffer_len,
      format, args);

  vterm_push_output_bytes(vt, vt->tmpbuffer, len);
}

INTERNAL void vterm_push_output_sprintf(VTerm *vt, const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vterm_push_output_vsprintf(vt, format, args);
  va_end(args);
}

INTERNAL void vterm_push_output_sprintf_ctrl(VTerm *vt, unsigned char ctrl, const char *fmt, ...)
{
  size_t cur;

  if(ctrl >= 0x80 && !vt->mode.ctrl8bit)
    cur = snprintf(vt->tmpbuffer, vt->tmpbuffer_len,
        ESC_S "%c", ctrl - 0x40);
  else
    cur = snprintf(vt->tmpbuffer, vt->tmpbuffer_len,
        "%c", ctrl);

  if(cur >= vt->tmpbuffer_len)
    return;

  va_list args;
  va_start(args, fmt);
  cur += vsnprintf(vt->tmpbuffer + cur, vt->tmpbuffer_len - cur,
      fmt, args);
  va_end(args);

  if(cur >= vt->tmpbuffer_len)
    return;

  vterm_push_output_bytes(vt, vt->tmpbuffer, cur);
}

INTERNAL void vterm_push_output_sprintf_dcs(VTerm *vt, const char *fmt, ...)
{
  size_t cur = 0;

  cur += snprintf(vt->tmpbuffer + cur, vt->tmpbuffer_len - cur,
      vt->mode.ctrl8bit ? "\x90" : ESC_S "P"); // DCS

  if(cur >= vt->tmpbuffer_len)
    return;

  va_list args;
  va_start(args, fmt);
  cur += vsnprintf(vt->tmpbuffer + cur, vt->tmpbuffer_len - cur,
      fmt, args);
  va_end(args);

  if(cur >= vt->tmpbuffer_len)
    return;

  cur += snprintf(vt->tmpbuffer + cur, vt->tmpbuffer_len - cur,
      vt->mode.ctrl8bit ? "\x9C" : ESC_S "\\"); // ST

  if(cur >= vt->tmpbuffer_len)
    return;

  vterm_push_output_bytes(vt, vt->tmpbuffer, cur);
}

size_t vterm_output_get_buffer_size(const VTerm *vt)
{
  return vt->outbuffer_len;
}

size_t vterm_output_get_buffer_current(const VTerm *vt)
{
  return vt->outbuffer_cur;
}

size_t vterm_output_get_buffer_remaining(const VTerm *vt)
{
  return vt->outbuffer_len - vt->outbuffer_cur;
}

size_t vterm_output_read(VTerm *vt, char *buffer, size_t len)
{
  if(len > vt->outbuffer_cur)
    len = vt->outbuffer_cur;

  memcpy(buffer, vt->outbuffer, len);

  if(len < vt->outbuffer_cur)
    memmove(vt->outbuffer, vt->outbuffer + len, vt->outbuffer_cur - len);

  vt->outbuffer_cur -= len;

  return len;
}

VTermValueType vterm_get_attr_type(VTermAttr attr)
{
  switch(attr) {
    case VTERM_ATTR_BOLD:       return VTERM_VALUETYPE_BOOL;
    case VTERM_ATTR_UNDERLINE:  return VTERM_VALUETYPE_INT;
    case VTERM_ATTR_ITALIC:     return VTERM_VALUETYPE_BOOL;
    case VTERM_ATTR_BLINK:      return VTERM_VALUETYPE_BOOL;
    case VTERM_ATTR_REVERSE:    return VTERM_VALUETYPE_BOOL;
    case VTERM_ATTR_STRIKE:     return VTERM_VALUETYPE_BOOL;
    case VTERM_ATTR_FONT:       return VTERM_VALUETYPE_INT;
    case VTERM_ATTR_FOREGROUND: return VTERM_VALUETYPE_COLOR;
    case VTERM_ATTR_BACKGROUND: return VTERM_VALUETYPE_COLOR;

    case VTERM_N_ATTRS: return 0;
  }
  return 0; /* UNREACHABLE */
}

VTermValueType vterm_get_prop_type(VTermProp prop)
{
  switch(prop) {
    case VTERM_PROP_CURSORVISIBLE: return VTERM_VALUETYPE_BOOL;
    case VTERM_PROP_CURSORBLINK:   return VTERM_VALUETYPE_BOOL;
    case VTERM_PROP_ALTSCREEN:     return VTERM_VALUETYPE_BOOL;
    case VTERM_PROP_TITLE:         return VTERM_VALUETYPE_STRING;
    case VTERM_PROP_ICONNAME:      return VTERM_VALUETYPE_STRING;
    case VTERM_PROP_REVERSE:       return VTERM_VALUETYPE_BOOL;
    case VTERM_PROP_CURSORSHAPE:   return VTERM_VALUETYPE_INT;
    case VTERM_PROP_MOUSE:         return VTERM_VALUETYPE_INT;

    case VTERM_N_PROPS: return 0;
  }
  return 0; /* UNREACHABLE */
}

void vterm_scroll_rect(VTermRect rect,
    int downward,
    int rightward,
    int (*moverect)(VTermRect src, VTermRect dest, void *user),
    int (*eraserect)(VTermRect rect, int selective, void *user),
    void *user)
{
  VTermRect src;
  VTermRect dest;


  if(abs(downward)  >= rect.end_row - rect.start_row ||
     abs(rightward) >= rect.end_col - rect.start_col) {
    /* Scroll more than area; just erase the lot */
    (*eraserect)(rect, 0, user);
    return;
  }


  if(rightward >= 0) {
    /* rect: [XXX................]
     * src:     [----------------]
     * dest: [----------------]
     */
    dest.start_col = rect.start_col;
    dest.end_col   = rect.end_col   - rightward;
    src.start_col  = rect.start_col + rightward;
    src.end_col    = rect.end_col;
  }
  else {
    /* rect: [................XXX]
     * src:  [----------------]
     * dest:    [----------------]
     */
    int leftward = -rightward;
    dest.start_col = rect.start_col + leftward;
    dest.end_col   = rect.end_col;
    src.start_col  = rect.start_col;
    src.end_col    = rect.end_col - leftward;
  }


  if(downward >= 0) {
    dest.start_row = rect.start_row;
    dest.end_row   = rect.end_row   - downward;
    src.start_row  = rect.start_row + downward;
    src.end_row    = rect.end_row;
  }
  else {
    int upward = -downward;
    dest.start_row = rect.start_row + upward;
    dest.end_row   = rect.end_row;
    src.start_row  = rect.start_row;
    src.end_row    = rect.end_row - upward;
  }


  if(moverect)
    (*moverect)(dest, src, user);


  if(downward > 0)
    rect.start_row = rect.end_row - downward;
  else if(downward < 0)
    rect.end_row = rect.start_row - downward;


  if(rightward > 0)
    rect.start_col = rect.end_col - rightward;
  else if(rightward < 0)
    rect.end_col = rect.start_col - rightward;


  (*eraserect)(rect, 0, user);
}

void vterm_copy_cells(VTermRect dest,
    VTermRect src,
    void (*copycell)(VTermPos dest, VTermPos src, void *user),
    void *user)
{
  int downward  = src.start_row - dest.start_row;
  int rightward = src.start_col - dest.start_col;

  int init_row, test_row, init_col, test_col;
  int inc_row, inc_col;

  if(downward < 0) {
    init_row = dest.end_row - 1;
    test_row = dest.start_row - 1;
    inc_row = -1;
  }
  else /* downward >= 0 */ {
    init_row = dest.start_row;
    test_row = dest.end_row;
    inc_row = +1;
  }

  if(rightward < 0) {
    init_col = dest.end_col - 1;
    test_col = dest.start_col - 1;
    inc_col = -1;
  }
  else /* rightward >= 0 */ {
    init_col = dest.start_col;
    test_col = dest.end_col;
    inc_col = +1;
  }

  VTermPos pos;
  for(pos.row = init_row; pos.row != test_row; pos.row += inc_row)
    for(pos.col = init_col; pos.col != test_col; pos.col += inc_col) {
      VTermPos srcpos = { pos.row + downward, pos.col + rightward };
      (*copycell)(pos, srcpos, user);
    }
}

/// begin stuff added by Ethan ///
//  mostly sourced from PangoTerm

// pty
static int master;

size_t write_master(const char *bytes, size_t len)
{
    return write(master, bytes, len);
}

static void pty_resized(int rows, int cols, void *user)
{
    struct winsize size = { rows, cols, 0, 0 };
    ioctl(master, TIOCSWINSZ, &size);
}

int open_master(int rows, int cols) {
    /* None of the docs about termios explain how to construct a new one of
 * these, so this is largely a guess */
    struct termios termios = {
            .c_iflag = ICRNL|IXON,
            .c_oflag = OPOST|ONLCR
#ifdef TAB0
            |TAB0
#endif
            ,
            .c_cflag = CS8|CREAD,
            .c_lflag = ISIG|ICANON|IEXTEN|ECHO|ECHOE|ECHOK,
            /* c_cc later */
    };

#ifdef IUTF8
    termios.c_iflag |= IUTF8;
#endif
#ifdef NL0
    termios.c_oflag |= NL0;
#endif
#ifdef CR0
    termios.c_oflag |= CR0;
#endif
#ifdef BS0
    termios.c_oflag |= BS0;
#endif
#ifdef VT0
    termios.c_oflag |= VT0;
#endif
#ifdef FF0
    termios.c_oflag |= FF0;
#endif
#ifdef ECHOCTL
    termios.c_lflag |= ECHOCTL;
#endif
#ifdef ECHOKE
    termios.c_lflag |= ECHOKE;
#endif

    cfsetspeed(&termios, 38400);

    termios.c_cc[VINTR]    = 0x1f & 'C';
    termios.c_cc[VQUIT]    = 0x1f & '\\';
    termios.c_cc[VERASE]   = 0x7f;
    termios.c_cc[VKILL]    = 0x1f & 'U';
    termios.c_cc[VEOF]     = 0x1f & 'D';
    termios.c_cc[VEOL]     = _POSIX_VDISABLE;
    termios.c_cc[VEOL2]    = _POSIX_VDISABLE;
    termios.c_cc[VSTART]   = 0x1f & 'Q';
    termios.c_cc[VSTOP]    = 0x1f & 'S';
    termios.c_cc[VSUSP]    = 0x1f & 'Z';
    termios.c_cc[VREPRINT] = 0x1f & 'R';
    termios.c_cc[VWERASE]  = 0x1f & 'W';
    termios.c_cc[VLNEXT]   = 0x1f & 'V';
    termios.c_cc[VMIN]     = 1;
    termios.c_cc[VTIME]    = 0;

    struct winsize size = { rows, cols, 0, 0 };

    int stderr_save_fileno = dup(2);

    pid_t kid = forkpty(&master, NULL, &termios, &size);

    if(kid == 0) {
        fcntl(stderr_save_fileno, F_SETFD, fcntl(stderr_save_fileno, F_GETFD) | FD_CLOEXEC);
        FILE *stderr_save = fdopen(stderr_save_fileno, "a");

        /* Restore the ISIG signals back to defaults */
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGSTOP, SIG_DFL);
        signal(SIGCONT, SIG_DFL);

        putenv("TERM=xterm");
        /* Do not free 'term', it is part of the environment */

        putenv("COLORTERM=truecolor");

        char *shell = getenv("SHELL");
        char *args[2] = { shell, NULL };
        execvp(shell, args);
        fprintf(stderr_save, "Cannot exec(%s) - %s\n", shell, strerror(errno));

        _exit(1);
    }

    close(stderr_save_fileno);

    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    return kid;
}

int master_read(VTerm *vt) {
    while(1) {
        /* Linux kernel's PTY buffer is a fixed 4096 bytes (1 page) so there's
         * never any point read()ing more than that
         */
        char buffer[4096];

        ssize_t bytes = read(master, buffer, sizeof buffer);

        if(bytes == -1 && errno == EAGAIN)
            break;

        if(bytes == 0 || (bytes == -1 && errno == EIO)) {
            return 0;
        }

        if(bytes < 0) {
            fprintf(stderr, " read(master) failed - %s\n", strerror(errno));
            exit(1);
        }

        vterm_input_write(vt, buffer, bytes);
    }
}

void vt_flush_outbuffer(VTerm *vt)
{
    if(vt->g_out->len) {
        write(master, vt->g_out->str, vt->g_out->len);
        vt->g_out->len = 0;
    }
}