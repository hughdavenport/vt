#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define UNIMPL(fmt, ...) do { fprintf(stderr, "%s:%d: \033[31mUNIMPLEMENTED\033[m: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); fflush(stderr); abort(); } while (false)
#define UNREACHABLE(fmt, ...) do { fprintf(stderr, "%s:%d: UNREACHABLE: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); fflush(stderr); abort(); } while (false)

#define C_ARRAY_LEN(arr) (sizeof((arr))/sizeof(*(arr)))

#define VT_STATES_LIST \
    X(VT_STATE_GROUND) \
    X(VT_STATE_ESCAPE) \
    X(VT_STATE_ESCAPE_INTERMEDIATE) \
    X(VT_STATE_OSC_STRING) \
    X(VT_STATE_CSI_ENTRY) \
    X(VT_STATE_CSI_PARAM) \
    X(VT_STATE_CSI_IGNORE) \
    X(VT_STATE_CSI_INTERMEDIATE) \
    X(VT_STATE_SOS_PM_APC_STRING) \
    X(VT_STATE_DCS_ENTRY) \
    X(VT_STATE_DCS_IGNORE) \
    X(VT_STATE_DCS_PARAM) \
    X(VT_STATE_DCS_INTERMEDIATE) \
    X(VT_STATE_DCS_PASSTHROUGH)

#define VT_ACTIONS_LIST \
    X(VT_ACTION_IGNORE)       S((void)NULL) \
    X(VT_ACTION_PRINT)        S(_vt_print(vt, input)) \
    X(VT_ACTION_EXECUTE)      S(_vt_execute(vt, input)) \
    X(VT_ACTION_CLEAR)        S(_vt_clear(vt)) \
    X(VT_ACTION_COLLECT)      S(_vt_collect(vt, input)) \
    X(VT_ACTION_PARAM)        S(_vt_param(vt, input)) \
    X(VT_ACTION_ESC_DISPATCH) S(_vt_escape_dispatch(vt, input)) \
    X(VT_ACTION_CSI_DISPATCH) S(_vt_csi_dispatch(vt, input)) \
    X(VT_ACTION_HOOK)         S(UNIMPL("VT_ACTION_HOOK")) \
    X(VT_ACTION_PUT)          S(UNIMPL("VT_ACTION_PUT")) \
    X(VT_ACTION_UNHOOK)       S(UNIMPL("VT_ACTION_UNHOOK")) \
    X(VT_ACTION_OSC_START)    S(UNIMPL("VT_ACTION_OSC_START")) \
    X(VT_ACTION_OSC_PUT)      S(UNIMPL("VT_ACTION_OSC_PUT")) \
    X(VT_ACTION_OSC_END)      S(UNIMPL("VT_ACTION_OSC_END"))

#define VT_ESCAPE_FUNCTIONS_LIST \
   C(0x00)         X(VT_ESCAPE_NONE)  L("NONE")                   S(UNREACHABLE("Unexpected Escape function")) \
   C(0x37 /* 7 */) X(VT_ESCAPE_DECSC) L("Save Cursor")            S(_vt_save_cursor(vt)) \
   C(0x4F /* O */) X(VT_ESCAPE_SS3)   L("Single Shift 3")         S(vt->sequence_state.shift = 3; vt->sequence_state.shift_lock = false) \
   C(0x63 /* c */) X(VT_ESCAPE_RIS)   L("Reset to Initial State") S(vt_free(vt); vt_resize_window(vt))

#define VT_CONTROL_FUNCTIONS_LIST \
   C(0x00) X(VT_CONTROL_NULL)  L("Null") S(UNIMPL("VT_CONTROL_NULL")) \
   C(0x03) X(VT_CONTROL_ETX)   L("End of Text") S(kill(getpid(), SIGINT)) \
   C(0x05) X(VT_CONTROL_ENQ)   L("Enquire") S(UNIMPL("VT_CONTROL_ENQ")) \
   C(0x07) X(VT_CONTROL_BEL)   L("Bell") S(_vt_bell(vt)) \
   C(0x08) X(VT_CONTROL_BS)    L("Backspace") S(_vt_backspace(vt)) \
   C(0x09) X(VT_CONTROL_HT)    L("Horizontal Tab") S(UNIMPL("VT_CONTROL_HT")) \
   C(0x0A) X(VT_CONTROL_LF)    L("Line Feed") S(_vt_line_feed(vt)) \
   C(0x0B) X(VT_CONTROL_VT)    L("Vertical Tab") S(UNIMPL("VT_CONTROL_VT")) \
   C(0x0C) X(VT_CONTROL_FF)    L("Form Feed") S(UNIMPL("VT_CONTROL_FF")) \
   C(0x0D) X(VT_CONTROL_CR)    L("Carriage Return") S(_vt_carriage_return(vt)) \
   C(0x0E) X(VT_CONTROL_SO)    L("Shift Out") S(UNIMPL("VT_CONTROL_SO")) \
   C(0x0F) X(VT_CONTROL_SI)    L("Shift Out") S(UNIMPL("VT_CONTROL_SI")) \
   C(0x11) X(VT_CONTROL_DC1)   L("Device Control 1 (XON)") S(UNIMPL("VT_CONTROL_DC1")) \
   C(0x13) X(VT_CONTROL_DC3)   L("Device Control 3 (XOFF)") S(UNIMPL("VT_CONTROL_DC3")) \
   C(0x18) X(VT_CONTROL_CAN)   L("Cancel") S(UNIMPL("VT_CONTROL_CAN")) \
   C(0x1A) X(VT_CONTROL_SUB)   L("Substitute") S(UNIMPL("VT_CONTROL_SUB")) \
   C(0x1B) X(VT_CONTROL_ESC)   L("Escape") S(UNIMPL("VT_CONTROL_ESC")) /* UNREACHABLE, mapped in "anywhere" transitions */ \
   C(0x1C) X(VT_CONTROL_GS)    L("Group Separator") S(UNIMPL("VT_CONTROL_GS")) \
   C(0x7F) X(VT_CONTROL_DEL)   L("Delete") S(UNIMPL("VT_CONTROL_DEL")) /* UNREACHABLE, used only in VT_ACTION_PRINT or VT_ACTION_IGNORE not VT_ACTION_EXECUTE */ \
   C(0x84) X(VT_CONTROL_IND)   L("Index") S(UNIMPL("VT_CONTROL_IND")) \
   C(0x85) X(VT_CONTROL_NEL)   L("Next Line") S(UNIMPL("VT_CONTROL_NEL")) \
   C(0x88) X(VT_CONTROL_HTS)   L("Horizontal Tab Set") S(UNIMPL("VT_CONTROL_HTS")) \
   C(0x8D) X(VT_CONTROL_RI)    L("Reverse Index") S(UNIMPL("VT_CONTROL_RI")) \
   C(0x8E) X(VT_CONTROL_SS2)   L("Single shift 2") S(vt->sequence_state.shift = 2; vt->sequence_state.shift_lock = false) \
   C(0x8F) X(VT_CONTROL_SS3)   L("Single shift 3") S(vt->sequence_state.shift = 3; vt->sequence_state.shift_lock = false) \
   C(0x90) X(VT_CONTROL_DCS)   L("Device Control String") S(UNIMPL("VT_CONTROL_DCS")) /* UNREACHABLE, mapping in "anywhere" transitions */ \
   C(0x98) X(VT_CONTROL_SOS)   L("Start Of String") S(UNIMPL("VT_CONTROL_SOS")) \
   C(0x9A) X(VT_CONTROL_DECID) L("DEC Private Identification") S(UNIMPL("VT_CONTROL_DECID")) \
   C(0x9B) X(VT_CONTROL_CSI)   L("Control Sequence Introducer") S(UNIMPL("VT_CONTROL_CSI")) /* UNREACHABLE, mapped in "anywhere" transitions */ \
   C(0x9C) X(VT_CONTROL_ST)    L("String Terminator") S(UNIMPL("VT_CONTROL_ST")) \
   C(0x9D) X(VT_CONTROL_OSC)   L("Operating System Command") S(UNIMPL("VT_CONTROL_OSC")) /* UNREACHABLE, mapped in "anywhere" transitions */ \
   C(0x9E) X(VT_CONTROL_PM)    L("Privacy Message") S(UNIMPL("VT_CONTROL_PM")) \
   C(0x9F) X(VT_CONTROL_APC)   L("Application Program Command") S(UNIMPL("VT_CONTROL_APC"))

#define VT_PARAM(vt, idx, def) ((idx) < (vt)->num_params && (vt)->params[(idx)].non_default ? (vt)->params[(idx)].value : (def))

#define VT_CSI_FUNCTIONS_LIST \
   C(0x00)         X(VT_CSI_NONE)          L("NONE")              S(UNREACHABLE("Unexpected CSI function")) \
   C(0x41 /* A */) X(VT_CSI_CUU)           L("Cursor Up")         S(_vt_move_cursor_offset(vt, 0, -VT_PARAM(vt, 0, 1))) \
   C(0x42 /* B */) X(VT_CSI_CUD)           L("Cursor Down")       S(_vt_move_cursor_offset(vt, 0, VT_PARAM(vt, 0, 1))) \
   C(0x43 /* C */) X(VT_CSI_CUF)           L("Cursor Forward")    S(_vt_move_cursor_offset(vt, VT_PARAM(vt, 0, 1), 0)) \
   C(0x44 /* D */) X(VT_CSI_CUB)           L("Cursor Backward")   S(_vt_move_cursor_offset(vt, -VT_PARAM(vt, 0, 1), 0)) \
   C(0x48 /* H */) X(VT_CSI_CUP)           L("Cursor Position")   S(_vt_move_cursor(vt, VT_PARAM(vt, 1, 1), VT_PARAM(vt, 0, 1))) \
   C(0x4A /* J */) X(VT_CSI_ED)            L("Erase In Page")     S(_vt_erase_in_page(vt, VT_PARAM(vt, 0, 0))) \
   C(0x4B /* K */) X(VT_CSI_EL)            L("Erase In Line")     S(_vt_erase_in_line(vt, VT_PARAM(vt, 0, 0))) \
   C(0x7E /* ~ */) X(VT_CSI_PRIVATE_TILDE) L("CSI Private Tilde") S(_vt_csi_private_tilde_dispatch(vt, VT_PARAM(vt, 0, 0)))

#define VT_CSI_PRIVATE_TILDE_FUNCTIONS_LIST \
   C(0x00) X(VT_CSI_PRIVATE_TILDE_NONE)   L("NONE")   S(UNREACHABLE("Unexpected CSI private tilde function")) \
   C(1)    X(VT_CSI_PRIVATE_TILDE_HOME)   L("Home")   S(UNIMPL("VT_CSI_PRIVATE_TILDE_HOME")) \
   C(2)    X(VT_CSI_PRIVATE_TILDE_INSERT) L("Insert") S(UNIMPL("VT_CSI_PRIVATE_TILDE_INSERT")) \
   C(3)    X(VT_CSI_PRIVATE_TILDE_DELETE) L("Delete") S(UNIMPL("VT_CSI_PRIVATE_TILDE_DELETE")) \
   C(4)    X(VT_CSI_PRIVATE_TILDE_END)    L("End")    S(UNIMPL("VT_CSI_PRIVATE_TILDE_END"))

#define VT_CSI_PRIVATE_QUESTION_FUNCTIONS_LIST \
   C(0x00) X(VT_CSI_PRIVATE_QUESTION_NONE)        L("NONE")                      S(UNREACHABLE("Unexpected CSI private question function")) \
   C(25)   X(VT_CSI_PRIVATE_QUESTION_DECTCEM)     L("Show Cursor")               S(fprintf(vt->tty, "\033[?25%c", input); fflush(vt->tty)) \
   C(47)   X(VT_CSI_PRIVATE_QUESTION_ALTBUF)      L("Alternative Screen Buffer") S(_vt_alternate_buffer(vt, input)) \
   C(1000) X(VT_CSI_PRIVATE_QUESTION_VT200_MOUSE) L("VT200 Mouse Reporting")     S(UNIMPL("VT_CSI_PRIVATE_VT200_MOUSE"))

#define C(code)
#define S(code)
#define L(code)

#define X(name) name, 
typedef enum { VT_STATES_LIST VT_NUM_STATES } vt_state;
typedef enum { VT_ACTIONS_LIST VT_NUM_ACTIONS } vt_action;
typedef enum { VT_ESCAPE_FUNCTIONS_LIST VT_NUM_ESCAPE_FUNCTIONS } vt_escape_function;
typedef enum { VT_CONTROL_FUNCTIONS_LIST VT_NUM_CONTROL_FUNCTIONS } vt_control_function;
typedef enum { VT_CSI_FUNCTIONS_LIST VT_NUM_CSI_FUNCTIONS } vt_csi_function;
typedef enum { VT_CSI_PRIVATE_QUESTION_FUNCTIONS_LIST VT_NUM_CSI_PRIVATE_QUESTION_FUNCTIONS } vt_csi_private_question_function;
typedef enum { VT_CSI_PRIVATE_TILDE_FUNCTIONS_LIST VT_NUM_CSI_PRIVATE_TILDE_FUNCTIONS } vt_csi_private_tilde_function;
#undef X

#define X(name) [name] = #name, 
static const char *vt_state_strings[] = { VT_STATES_LIST };
static const char *vt_action_strings[] = { VT_ACTIONS_LIST };
static const char *vt_escape_function_strings[] = { VT_ESCAPE_FUNCTIONS_LIST };
static const char *vt_control_function_strings[] = { VT_CONTROL_FUNCTIONS_LIST };
static const char *vt_csi_function_strings[] = { VT_CSI_FUNCTIONS_LIST };
static const char *vt_csi_private_question_function_strings[] = { VT_CSI_PRIVATE_QUESTION_FUNCTIONS_LIST };
static const char *vt_csi_private_tilde_function_strings[] = { VT_CSI_PRIVATE_TILDE_FUNCTIONS_LIST };

#define VT_STATE_STRING(stat) (((stat) >= 0 && (stat) < VT_NUM_STATES) ? vt_state_strings[(stat)] : "(state out of bounds)")
#define VT_ACTION_STRING(act) (((act) >= 0 && (act) < VT_NUM_ACTIONS) ? vt_action_strings[(act)] : "(action out of bounds)")
#define VT_ESCAPE_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_ESCAPE_FUNCTIONS) ? vt_escape_function_strings[(func)] : "(escape_function out of bounds)")
#define VT_CONTROL_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_CONTROL_FUNCTIONS) ? vt_control_function_strings[(func)] : "(control_function out of bounds)")
#define VT_CSI_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_CSI_FUNCTIONS) ? vt_csi_function_strings[(func)] : "(csi_function out of bounds)")
#define VT_CSI_PRIVATE_QUESTION_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_CSI_PRIVATE_QUESTION_FUNCTIONS) ? vt_csi_private_question_function_strings[(func)] : "(csi_private_question_function out of bounds)")
#define VT_CSI_PRIVATE_TILDE_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_CSI_PRIVATE_TILDE_FUNCTIONS) ? vt_csi_private_tilde_function_strings[(func)] : "(csi_private_tilde_function out of bounds)")
#undef X

#define X(code)
#undef L
#define L(name) name, 
static const char *vt_csi_function_strings_long[] = { VT_CSI_FUNCTIONS_LIST };
static const char *vt_csi_private_question_function_strings_long[] = { VT_CSI_PRIVATE_QUESTION_FUNCTIONS_LIST };
static const char *vt_csi_private_tilde_function_strings_long[] = { VT_CSI_PRIVATE_TILDE_FUNCTIONS_LIST };
static const char *vt_escape_function_strings_long[] = { VT_ESCAPE_FUNCTIONS_LIST };
static const char *vt_control_function_strings_long[] = { VT_CONTROL_FUNCTIONS_LIST };

#define VT_CSI_FUNCTION_STRING_LONG(func) (((func) >= 0 && (func) < VT_NUM_CSI_FUNCTIONS) ? vt_csi_function_strings_long[(func)] : "(csi_function out of bounds)")
#define VT_CSI_PRIVATE_QUESTION_FUNCTION_STRING_LONG(func) (((func) >= 0 && (func) < VT_NUM_CSI_PRIVATE_QUESTION_FUNCTIONS) ? vt_csi_private_question_function_strings_long[(func)] : "(csi_private_question_function out of bounds)")
#define VT_CSI_PRIVATE_TILDE_FUNCTION_STRING_LONG(func) (((func) >= 0 && (func) < VT_NUM_CSI_PRIVATE_TILDE_FUNCTIONS) ? vt_csi_private_tilde_function_strings_long[(func)] : "(csi_private_tilde_function out of bounds)")
#define VT_CONTROL_FUNCTION_STRING_LONG(func) (((func) >= 0 && (func) < VT_NUM_CONTROL_FUNCTIONS) ? vt_control_function_strings_long[(func)] : "(control_function out of bounds)")
#define VT_ESCAPE_FUNCTION_STRING_LONG(func) (((func) >= 0 && (func) < VT_NUM_ESCAPE_FUNCTIONS) ? vt_escape_function_strings_long[(func)] : "(escape_function out of bounds)")
#undef L
#undef X
#define L(name)

static_assert(VT_NUM_STATES == 14, "Not the same number as William's design");
static_assert(VT_NUM_ACTIONS == 14, "Not the same number as William's design");

typedef enum
{
    VT_ATTRIBUTE_NONE
} vt_attribute;

typedef struct
{
    bool used;
    char c;
    vt_attribute attribute;
} vt_cell;

typedef struct
{
   bool non_default;
   uint16_t value;
} vt_param;

typedef struct
{
    size_t x;
    size_t y;
    bool wrap_pending;
    vt_attribute current_attribute;
} vt_cursor;

typedef struct
{
    vt_cell *cells;
    vt_cursor cursor;
    vt_cursor *saved_cursor;
    size_t height;
    size_t width;
    bool dirty;
} vt_buffer;

typedef struct
{
    vt_param params[16];
    size_t num_params;
    char collected[2];
    size_t num_collected;
} vt_sequence_state;

typedef struct
{
    vt_state state;
    struct termios original_ios;
    bool raw;
    bool nonblocking;
    FILE *tty;
    struct winsize outer_window;
    vt_buffer primary_buffer;
    vt_buffer *alternate_buffer;
    vt_sequence_state sequence_state;
    pid_t child_pid;
    int stdin[2];
    int stdout[2];
} vt;

void vt_process(vt *vt, uint8_t input);


void _vt_print(vt *vt, char input)
{
    if (!vt) return;
    vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
    if (!buffer->cells) return;

    if (!isprint(input)) {
        if (input == 0x7F) {
           // depends what is mapped into GL whether this (and space 0x20 which may differ in another set but defaults to ' ') is printed or not
           return;
        }
        UNREACHABLE("Unexpected non printable char 0x%02X", input);
    }

    if (buffer->cursor.wrap_pending) {
        if (buffer->cursor.y == buffer->height) {
           fprintf(stderr, "screen wrap\n");
            _vt_scroll(vt, VT_SCROLL_UP);
        } else {
           fprintf(stderr, "line wrap\n");
        }
        buffer->cursor.y ++;
        buffer->cursor.x = 1;
        buffer->cursor.wrap_pending = false;
    }

    vt_cell *cell = &buffer->cells[(buffer->cursor.y - 1) * buffer->width + buffer->cursor.x - 1];
    cell->used = true;
    cell->c = input;
    cell->attribute = buffer->cursor.current_attribute;

    if (buffer->cursor.x == buffer->width) {
       fprintf(stderr, "line wrap pending\n");
        buffer->cursor.wrap_pending = true;
    } else {
        buffer->cursor.x ++;
    }

    buffer->dirty = true;
}

void _vt_clear(vt *vt)
{
    /* UNIMPL("sizeof(vt_sequence_state) = %zu", sizeof(vt_sequence_state)); */
    static_assert(sizeof(vt_sequence_state) == 104, "State added, may need clearing");
    if (!vt) return;

    memset(&vt->sequence_state, '\0', sizeof(vt->sequence_state));
}

void _vt_collect(vt *vt, uint8_t input)
{
   if (!vt) return;

   if (vt->sequence_state.num_collected == C_ARRAY_LEN(vt->sequence_state.collected)) {
      UNIMPL("Collect more than %lu things", C_ARRAY_LEN(vt->sequence_state.collected));
   }
   vt->sequence_state.collected[vt->sequence_state.num_collected++] = input;
}

void _vt_param(vt *vt, uint8_t input)
{
   if (!vt) return;

   vt_param *param = NULL;
   if (isdigit(input)) {
      if (vt->num_params != C_ARRAY_LEN(vt->params)) {
          if (vt->num_params) {
             param = &vt->params[vt->num_params - 1];
             param->value *= 10;
             param->value += input - '0';
          } else {
             param = &vt->params[vt->num_params ++];
             param->value = input - '0';
          }
      }
      if (param) param->non_default = true;
   } else if (vt->num_params != C_ARRAY_LEN(vt->params)) {
      param = &vt->params[vt->num_params ++];
      param->non_default = false;
   }

   if (param) {
      fprintf(stderr, "in _vt_param, input %02X (%c), num params %zu, param so far %d (default=%d)\n", input, input, vt->num_params, param->value, !param->non_default);
   } else {
      fprintf(stderr, "in _vt_param, input %02X (%c), num params %zu, ignoring more\n", input, input, vt->num_params);
   }
}

#define X(name) case name:
#define S(code) code; return;
void _vt_execute(vt *vt, uint8_t input)
{
    if (!vt) return;

#define S(code)
#define C(code) case code:

#define X(name) func = name; break;
    vt_control_function func = -1;
    switch (input) { VT_CONTROL_FUNCTIONS_LIST }
    if ((signed)func == -1) UNIMPL("Unknown control function input 0x%02X '%c'", input, input);
#undef S
#undef C
#undef X

    fprintf(stderr, "state %s, execute %s (%s)\n", VT_STATE_STRING(vt->state), VT_CONTROL_FUNCTION_STRING(func), VT_CONTROL_FUNCTION_STRING_LONG(func));

#define C(code)
#define X(name) case name:
#define S(code) code; return;
    /* all cases must return */
    static_assert(VT_NUM_CONTROL_FUNCTIONS == 33, "Not all functions handled");
    switch (func) {
        VT_CONTROL_FUNCTIONS_LIST
        case VT_NUM_CONTROL_FUNCTIONS: break;
    }
    UNREACHABLE("Unexpected func %d", func);
#undef X
#undef S
#undef C
}

void _vt_escape_dispatch(vt *vt, uint8_t input)
{
    if (!vt) return;

#define S(code)
#define C(code) case code:
#define X(name) func = name; break;
    vt_escape_function func = -1;
    switch (input) { VT_ESCAPE_FUNCTIONS_LIST }
    if ((signed)func == -1) UNIMPL("Unknown escape function input 0x%02X '%c'", input, input);
#undef C
#undef S
#undef X

    fprintf(stderr, "state %s, escape function %s (%s)\n", VT_STATE_STRING(vt->state), VT_ESCAPE_FUNCTION_STRING(func), VT_ESCAPE_FUNCTION_STRING_LONG(func));

#define C(name)
#define X(name) case name:
#define S(code) code; return;
    /* all cases must return */
    static_assert(VT_NUM_ESCAPE_FUNCTIONS == 4, "Not all functions handled");
    switch (func) {
        VT_ESCAPE_FUNCTIONS_LIST
        case VT_NUM_ESCAPE_FUNCTIONS: break;
    }
    UNREACHABLE("Unexpected func %d", func);
#undef X
#undef S
#undef C
}

void _vt_csi_private_tilde_dispatch(vt *vt, uint16_t param)
{
    if (!vt) return;

#define S(code)
#define C(code) case code:
#define K(code)
#define X(name) func = name; break;
    vt_csi_private_tilde_function func = -1;
    switch (param) { VT_CSI_PRIVATE_TILDE_FUNCTIONS_LIST }
    if ((signed)func == -1) UNIMPL("Unknown csi private tilde function param %d", param);
#undef C
#undef S
#undef X

    fprintf(stderr, "state %s, csi private tilde %s (%s)\n", VT_STATE_STRING(vt->state),VT_CSI_PRIVATE_TILDE_FUNCTION_STRING(func), VT_CSI_PRIVATE_TILDE_FUNCTION_STRING_LONG(func));
    for (size_t param = 0; param < vt->sequence_state.num_params; param++) {
       if (vt->sequence_state.params[param].non_default) {
          if (param) fputc(',', stderr);
          fprintf(stderr, " %u", VT_PARAM(vt, param, 0));
       }
    }
    fprintf(stderr, "\n");

#define C(name)
#define X(name) case name:
#define S(code) code; return;
    /* all cases must return */
    static_assert(VT_NUM_CSI_PRIVATE_TILDE_FUNCTIONS == 5, "Not all functions handled");
    switch (func) {
       VT_CSI_PRIVATE_TILDE_FUNCTIONS_LIST
       case VT_NUM_CSI_PRIVATE_TILDE_FUNCTIONS: break;
    }
    UNREACHABLE("Unexpected csi private tilde func %d", func);
#undef X
#undef S
#undef C
}

void _vt_csi_private_question_dispatch(vt *vt, uint16_t param, uint8_t input)
{
    if (!vt) return;

#define S(code)
#define C(code) case code:
#define X(name) func = name; break;
    vt_csi_private_question_function func = -1;
    switch (param) { VT_CSI_PRIVATE_QUESTION_FUNCTIONS_LIST }
    if ((signed)func == -1) UNIMPL("Unknown csi private question function param %d", param);
#undef C
#undef S
#undef X

    fprintf(stderr, "state %s, csi private question %s %s (%s)\n", VT_STATE_STRING(vt->state), input == 'h' ? "enable" : (input == 'l' ? "disable" : "unknown"), VT_CSI_PRIVATE_QUESTION_FUNCTION_STRING(func), VT_CSI_PRIVATE_QUESTION_FUNCTION_STRING_LONG(func));
    for (size_t param = 0; param < vt->sequence_state.num_params; param++) {
       if (vt->sequence_state.params[param].non_default) {
          if (param) fputc(',', stderr);
          fprintf(stderr, " %u", VT_PARAM(vt, param, 0));
       }
    }
    fprintf(stderr, "\n");

    if (input != 'h' && input != 'l') UNREACHABLE("Invalid CSI terminator for private sequence"); 

#define C(name)
#define X(name) case name:
#define S(code) code; return;
    /* all cases must return */
    static_assert(VT_NUM_CSI_PRIVATE_QUESTION_FUNCTIONS == 4, "Not all functions handled");
    switch (func) {
       VT_CSI_PRIVATE_QUESTION_FUNCTIONS_LIST
       case VT_NUM_CSI_PRIVATE_QUESTION_FUNCTIONS: break;
    }
    UNREACHABLE("Unexpected csi private question func %d", func);
#undef X
#undef S
#undef C
}

void _vt_csi_dispatch(vt *vt, uint8_t input)
{
    if (!vt) return;

    if (vt->sequence_state.num_collected) {
       if (vt->sequence_state.num_collected == 1 && vt->sequence_state.collected[0] == '?') {
          _vt_csi_private_question_dispatch(vt, VT_PARAM(vt, 0, 0), input);
          return;
       } else {
          UNIMPL("CSI non private collected string");
       }
    }

#define S(code)
#define C(code) case code:
#define X(name) func = name; break;
    vt_csi_function func = -1;
    switch (input) { VT_CSI_FUNCTIONS_LIST }
    if ((signed)func == -1) UNIMPL("Unknown csi function input 0x%02X '%c'", input, input);
#undef C
#undef S
#undef X

    fprintf(stderr, "state %s, csi function %s (%s), args:", VT_STATE_STRING(vt->state), VT_CSI_FUNCTION_STRING(func), VT_CSI_FUNCTION_STRING_LONG(func));
    for (size_t param = 0; param < vt->sequence_state.num_params; param++) {
       if (vt->sequence_state.params[param].non_default) {
          if (param) fputc(',', stderr);
          fprintf(stderr, " %u", VT_PARAM(vt, param, 0));
       }
    }
    fprintf(stderr, "\n");

#define C(name)
#define X(name) case name:
#define S(code) code; return;
    /* all cases must return */
    static_assert(VT_NUM_CSI_FUNCTIONS == 9, "Not all functions handled");
    switch (func) {
        VT_CSI_FUNCTIONS_LIST
        case VT_NUM_CSI_FUNCTIONS: break;
    }
    UNREACHABLE("Unexpected func %d", func);
#undef X
#undef S
#undef C
#undef K
}

void _vt_action(vt *vt, vt_action action, uint8_t input)
{
    if (!vt) return;
    vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
    if (!buffer->cells) return;

    /* fprintf(stderr, "state %s, action %s, input ", VT_STATE_STRING(vt->state), VT_ACTION_STRING(action)); */
    /* vt_fprintc(stderr, input); */
    /* fprintf(stderr, ", cell %lux%lu", buffer->cursor.x, buffer->cursor.y); */
    /* if (buffer == vt->alternate_buffer) fprintf(stderr, " on alternate buffer"); */
    /* fprintf(stderr, "\n"); */

#define C(name)
#define X(name) case name:
#define S(code) code; return;
    /* all cases must return */
    static_assert(VT_NUM_ACTIONS == 14, "Not all functions handled");
    switch (action) {
        VT_ACTIONS_LIST
        case VT_NUM_ACTIONS: break;
    }
    UNREACHABLE("Unexpected action %d", action);
#undef X
#undef S
#undef C
}

void _vt_transition(vt *vt, vt_state to, uint8_t input)
{
    if (!vt) return;

    fprintf(stderr, "transition from state %s to state %s, input ", VT_STATE_STRING(vt->state), VT_STATE_STRING(to));
    vt_fprintc(stderr, input);
    fprintf(stderr, "\n");

    /* exit event */
    static_assert(VT_NUM_STATES == 14, "Not all states handled");
    switch (vt->state) {
        case VT_STATE_OSC_STRING: _vt_action(vt, VT_ACTION_OSC_END, 0); break;
        case VT_STATE_DCS_PASSTHROUGH: _vt_action(vt, VT_ACTION_UNHOOK, 0); break;

        default: break;
    }

    vt->state = to;

    /* entry event */
    static_assert(VT_NUM_STATES == 14, "Not all states handled");
    switch (vt->state) {
        case VT_STATE_ESCAPE:
        case VT_STATE_DCS_ENTRY:
        case VT_STATE_CSI_ENTRY:
            _vt_action(vt, VT_ACTION_CLEAR, 0);
            break;

        case VT_STATE_OSC_STRING: _vt_action(vt, VT_ACTION_OSC_START, 0); break;
        case VT_STATE_DCS_PASSTHROUGH: _vt_action(vt, VT_ACTION_UNHOOK, 0); break;

        default: break;
    }
}

void vt_process(vt *vt, uint8_t input)
{
    if (!vt) return;

    /* codes in GR (0xA0-0xFF) map to GL (0x20-0x7F) */
    if (input >= 0xA0) input &= 0x7F;

    /* anywhere state transistions */
    switch (input) {
        case 0x18: case 0x1A: case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: case 0x9C:
            _vt_action(vt, input == 0x9C ? VT_ACTION_IGNORE : VT_ACTION_EXECUTE, input);
            _vt_transition(vt, VT_STATE_GROUND, input);
            return;

        case 0x1B: _vt_transition(vt, VT_STATE_ESCAPE, input); return;
        case 0x90: _vt_transition(vt, VT_STATE_DCS_ENTRY, input); return;

        case 0x98: case 0x9E: case 0x9F:
            _vt_transition(vt, VT_STATE_SOS_PM_APC_STRING, input);
            return;

        case 0x9B: _vt_transition(vt, VT_STATE_CSI_ENTRY, input); return;
        case 0x9D: _vt_transition(vt, VT_STATE_OSC_STRING, input); return;
    }

#define NUM_00_0F N(0x00) N(0x01) N(0x02) N(0x03) N(0x04) N(0x05) N(0x06) N(0x07) N(0x08) N(0x09) N(0x0A) N(0x0B) N(0x0C) N(0x0D) N(0x0e) N(0x0F)
#define NUM_10_1F N(0x10) N(0x11) N(0x12) N(0x13) N(0x14) N(0x15) N(0x16) N(0x17) N(0x18) N(0x19) N(0x1A) N(0x1B) N(0x1C) N(0x1D) N(0x1e) N(0x1F)
#define NUM_20_2F N(0x20) N(0x21) N(0x22) N(0x23) N(0x24) N(0x25) N(0x26) N(0x27) N(0x28) N(0x29) N(0x2A) N(0x2B) N(0x2C) N(0x2D) N(0x2e) N(0x2F)
#define NUM_30_3F N(0x30) N(0x31) N(0x32) N(0x33) N(0x34) N(0x35) N(0x36) N(0x37) N(0x38) N(0x39) N(0x3A) N(0x3B) N(0x3C) N(0x3D) N(0x3e) N(0x3F)
#define NUM_40_4F N(0x40) N(0x41) N(0x42) N(0x43) N(0x44) N(0x45) N(0x46) N(0x47) N(0x48) N(0x49) N(0x4A) N(0x4B) N(0x4C) N(0x4D) N(0x4e) N(0x4F)
#define NUM_50_5F N(0x50) N(0x51) N(0x52) N(0x53) N(0x54) N(0x55) N(0x56) N(0x57) N(0x58) N(0x59) N(0x5A) N(0x5B) N(0x5C) N(0x5D) N(0x5e) N(0x5F)
#define NUM_60_6F N(0x60) N(0x61) N(0x62) N(0x63) N(0x64) N(0x65) N(0x66) N(0x67) N(0x68) N(0x69) N(0x6A) N(0x6B) N(0x6C) N(0x6D) N(0x6e) N(0x6F)
#define NUM_70_7F N(0x70) N(0x71) N(0x72) N(0x73) N(0x74) N(0x75) N(0x76) N(0x77) N(0x78) N(0x79) N(0x7A) N(0x7B) N(0x7C) N(0x7D) N(0x7e) N(0x7F)
#define NUM_80_8F N(0x80) N(0x81) N(0x82) N(0x83) N(0x84) N(0x85) N(0x86) N(0x87) N(0x88) N(0x89) N(0x8A) N(0x8B) N(0x8C) N(0x8D) N(0x8e) N(0x8F)
#define NUM_90_9F N(0x90) N(0x91) N(0x92) N(0x93) N(0x94) N(0x95) N(0x96) N(0x97) N(0x98) N(0x99) N(0x9A) N(0x9B) N(0x9C) N(0x9D) N(0x9e) N(0x9F)
#define NUM_A0_AF N(0xA0) N(0xA1) N(0xA2) N(0xA3) N(0xA4) N(0xA5) N(0xA6) N(0xA7) N(0xA8) N(0xA9) N(0xAA) N(0xAB) N(0xAC) N(0xAD) N(0xAE) N(0xAF)
#define NUM_B0_BF N(0xB0) N(0xB1) N(0xB2) N(0xB3) N(0xB4) N(0xB5) N(0xB6) N(0xB7) N(0xB8) N(0xB9) N(0xBA) N(0xBB) N(0xBC) N(0xBD) N(0xBE) N(0xBF)
#define NUM_C0_CF N(0xC0) N(0xC1) N(0xC2) N(0xC3) N(0xC4) N(0xC5) N(0xC6) N(0xC7) N(0xC8) N(0xC9) N(0xCA) N(0xCB) N(0xCC) N(0xCD) N(0xCE) N(0xCF)
#define NUM_D0_DF N(0xD0) N(0xD1) N(0xD2) N(0xD3) N(0xD4) N(0xD5) N(0xD6) N(0xD7) N(0xD8) N(0xD9) N(0xDA) N(0xDB) N(0xDC) N(0xDD) N(0xDE) N(0xDF)
#define NUM_E0_EF N(0xE0) N(0xE1) N(0xE2) N(0xE3) N(0xE4) N(0xE5) N(0xE6) N(0xE7) N(0xE8) N(0xE9) N(0xEA) N(0xEB) N(0xEC) N(0xED) N(0xEE) N(0xEF)
#define NUM_F0_FF N(0xF0) N(0xF1) N(0xF2) N(0xF3) N(0xF4) N(0xF5) N(0xF6) N(0xF7) N(0xF8) N(0xF9) N(0xFA) N(0xFB) N(0xFC) N(0xFD) N(0xFE) N(0xFF)

#define N(num) case num: 
    /* all cases must return */
    static_assert(VT_NUM_STATES == 14, "Not all states handled");
    switch (vt->state) {
        case VT_STATE_GROUND:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                NUM_20_2F NUM_30_3F NUM_40_4F NUM_50_5F NUM_60_6F NUM_70_7F
                    _vt_action(vt, VT_ACTION_PRINT, input);
                    break;
            }
            return;

        case VT_STATE_ESCAPE:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                NUM_20_2F
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_ESCAPE_INTERMEDIATE, input);
                    break;

                NUM_30_3F NUM_40_4F
                case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x59: case 0x5A:
                    _vt_action(vt, VT_ACTION_ESC_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND, input);
                    break;

                case 0x50: _vt_transition(vt, VT_STATE_DCS_ENTRY, input); break;
                case 0x5D: _vt_transition(vt, VT_STATE_OSC_STRING, input); break;
                case 0x5B: _vt_transition(vt, VT_STATE_CSI_ENTRY, input); break;

                case 0x58: case 0x5E: case 0x5F:
                    _vt_transition(vt, VT_STATE_SOS_PM_APC_STRING, input);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_ESCAPE_INTERMEDIATE:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                NUM_20_2F
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    break;

                NUM_30_3F NUM_40_4F NUM_50_5F NUM_60_6F
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
                    _vt_action(vt, VT_ACTION_ESC_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND, input);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_OSC_STRING:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                NUM_20_2F NUM_30_3F NUM_40_4F NUM_50_5F NUM_60_6F NUM_70_7F
                    _vt_action(vt, VT_ACTION_OSC_PUT, input);
                    break;

                case 0x9C: _vt_transition(vt, VT_STATE_GROUND, input); break;
            }
            return;

        case VT_STATE_CSI_ENTRY:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                NUM_20_2F
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_CSI_INTERMEDIATE, input);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3B:
                    _vt_action(vt, VT_ACTION_PARAM, input);
                    _vt_transition(vt, VT_STATE_CSI_PARAM, input);
                    break;

                case 0x3A: _vt_transition(vt, VT_STATE_CSI_IGNORE, input); break;

                case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_CSI_PARAM, input);
                    break;

                NUM_40_4F NUM_50_5F NUM_60_6F
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
                    _vt_action(vt, VT_ACTION_CSI_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND, input);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_CSI_PARAM:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                NUM_20_2F
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_CSI_INTERMEDIATE, input);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3B:
                    _vt_action(vt, VT_ACTION_PARAM, input);
                    break;

                case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_transition(vt, VT_STATE_CSI_IGNORE, input);
                    break;

                NUM_40_4F NUM_50_5F NUM_60_6F
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
                    _vt_action(vt, VT_ACTION_CSI_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND, input);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_CSI_IGNORE:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                NUM_20_2F NUM_30_3F
                case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                NUM_40_4F NUM_50_5F NUM_60_6F
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
                    _vt_transition(vt, VT_STATE_GROUND, input);
                    break;
            }
            return;

        case VT_STATE_CSI_INTERMEDIATE:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                NUM_20_2F
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    break;

                NUM_30_3F
                    _vt_transition(vt, VT_STATE_CSI_IGNORE, input);
                    break;

                NUM_40_4F NUM_50_5F NUM_60_6F
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
                    _vt_action(vt, VT_ACTION_CSI_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND, input);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_SOS_PM_APC_STRING:
            switch (input) {
                NUM_00_0F NUM_40_4F NUM_50_5F NUM_60_6F NUM_70_7F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                case 0x9C: _vt_transition(vt, VT_STATE_GROUND, input); break;
            }
            return;

        case VT_STATE_DCS_ENTRY:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                NUM_20_2F
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_DCS_INTERMEDIATE, input);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3B:
                    _vt_action(vt, VT_ACTION_PARAM, input);
                    _vt_transition(vt, VT_STATE_DCS_PARAM, input);
                    break;

                case 0x3A: _vt_transition(vt, VT_STATE_DCS_IGNORE, input); break;

                case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_DCS_PARAM, input);
                    break;

                NUM_40_4F NUM_50_5F NUM_60_6F
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
                    _vt_transition(vt, VT_STATE_DCS_PASSTHROUGH, input);
                    break;
            }
            return;

        case VT_STATE_DCS_IGNORE:
            switch (input) {
                NUM_00_0F NUM_20_2F NUM_30_3F NUM_40_4F NUM_50_5F NUM_60_6F NUM_70_7F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                case 0x9C: _vt_transition(vt, VT_STATE_GROUND, input); break;
            }
            return;

        case VT_STATE_DCS_PARAM:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                NUM_20_2F
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_DCS_INTERMEDIATE, input);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3B:
                    _vt_action(vt, VT_ACTION_PARAM, input);
                    break;

                case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_transition(vt, VT_STATE_DCS_IGNORE, input);
                    break;

                NUM_40_4F NUM_50_5F NUM_60_6F
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:

                    _vt_transition(vt, VT_STATE_DCS_PASSTHROUGH, input);
                    break;
            }
            return;

        case VT_STATE_DCS_INTERMEDIATE:
            switch (input) {
                NUM_00_0F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                NUM_20_2F
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    break;

                NUM_30_3F
                    _vt_transition(vt, VT_STATE_DCS_IGNORE, input);
                    break;

                NUM_40_4F NUM_50_5F NUM_60_6F
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:

                    _vt_transition(vt, VT_STATE_DCS_PASSTHROUGH, input);
                    break;
            }
            return;

        case VT_STATE_DCS_PASSTHROUGH:
            switch (input) {
                NUM_00_0F NUM_20_2F NUM_30_3F NUM_40_4F NUM_50_5F NUM_60_6F
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
                    _vt_action(vt, VT_ACTION_PUT, input);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;

                case 0x9C: _vt_transition(vt, VT_STATE_GROUND, input); break;
            }
            return;

        case VT_NUM_STATES: break;
    }
#undef N
    UNREACHABLE("Unexpected state %d", vt->state);
}

int vt_setup_io(vt *vt)
{
    if (!vt) return -1;

    if (isatty(STDOUT_FILENO)) {
        fprintf(stderr, "tty is stdout\n");
        vt->tty = stdout;
    } else if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "tty is stderr\n");
        vt->tty = stderr;
    } else {
        fprintf(stderr, "Opening /dev/tty\n");
        vt->tty = fopen("/dev/tty", "wb");
    }

    if (tcgetattr(STDIN_FILENO, &vt->original_ios) == 0) {
        struct termios new_ios = vt->original_ios;
        cfmakeraw(&new_ios);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &new_ios) == 0) {
            vt->raw = true;
        } else {
            tcsetattr(STDIN_FILENO, TCSANOW, &vt->original_ios);
        }
    }

    int fl = fcntl(STDIN_FILENO, F_GETFL);
    if (fl != -1 && !(fl & O_NONBLOCK)) {
        if (fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK) == 0) {
            vt->nonblocking = true;
        }
    }

    return 0;
}

void vt_restore_io(vt *vt)
{
    if (!vt) return;

    if (vt->tty && vt->tty != stdout && vt->tty != stderr) {
        fclose(vt->tty);
        vt->tty = NULL;
    }

    if (vt->raw) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &vt->original_ios) == 0) {
            vt->raw = false;
        }
    }

    if (vt->nonblocking) {
        int fl = fcntl(STDIN_FILENO, F_GETFL);
        if (fl != -1) {
            if (fcntl(STDIN_FILENO, F_SETFL, fl & ~O_NONBLOCK) == 0) {
                vt->nonblocking = false;
            }
        }
    }
}

void vt_draw_window(vt *vt)
{
#define VT_ED "\033[%dJ"
#define VT_CUP "\033[%d;%dH"

#define CLEAR_SCREEN fprintf(vt->tty, VT_ED, 2)
#define GOTO(x, y) fprintf(vt->tty, VT_CUP, (int)(y), (int)(x))

    if (!vt) return;

    if (!vt->dirty) return;

    CLEAR_SCREEN;
    for (int y = 10; y <= vt->window.ws_col; y += 10) {
        GOTO(3 + y, 1);
        fprintf(vt->tty, "%d", (y / 10) % 10);
    }
    GOTO(4, 2);
    for (int y = 1; y <= vt->window.ws_col; y ++) {
        fprintf(vt->tty, "%d", y % 10);
    }
    GOTO(3, 3);
    fprintf(vt->tty, "+");
    for (int y = 1; y <= vt->window.ws_col; y ++) {
        fprintf(vt->tty, "-");
    }
    for (int x = 1; x <= vt->window.ws_row; x ++) {
        GOTO(1, 3 + x);
        int mod = x % 10;
        if (mod) {
            fprintf(vt->tty, " %d|", mod);
        } else {
            fprintf(vt->tty, "%02d|", x % 100);
        }
    }

    vt_attribute last_attribute = VT_ATTRIBUTE_NONE;

    for (int y = 1; y <= vt->window.ws_row; y ++) {
        for (int x = 1; x <= vt->window.ws_col; x ++) {
            vt_cell *cell = &vt->cells[(y - 1) * vt->window.ws_col + x - 1];
            if (cell->used) {
                if (cell->attribute != last_attribute) {
                    UNIMPL("set attribute");
                }
                if (x == 1 || !vt->cells[(y - 1) * vt->window.ws_col + x - 2].used) {
                    GOTO(x + 3, y + 3);
                }
                fprintf(vt->tty, "%c", cell->c);
            }
        }
    }

    GOTO(vt->cursor.x + 3, vt->cursor.y + 3);

    fflush(vt->tty);
    vt->dirty = false;
}

void vt_resize_window(vt *vt)
{
    vt_cell *original = vt->cells;
    struct winsize original_size = vt->window;
    size_t orig_size = original_size.ws_col * original_size.ws_row;

    struct winsize w = {0};
    int fds[] = { STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO };
    for (size_t i = 0; i < C_ARRAY_LEN(fds); i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &w) == 0 && w.ws_col && w.ws_row)
            break;
        w = (struct winsize){0};
    }

    fprintf(stderr, "outer window size %ux%u\r\n", w.ws_col, w.ws_row);

    if (w.ws_col > 3 && w.ws_row > 3) {
        vt->window.ws_col = w.ws_col - 3;
        vt->window.ws_row = w.ws_row - 3;
    } else {
        vt->window = (struct winsize){.ws_col = 80, .ws_row = 24};
    }

    fprintf(stderr, "inner window size %ux%u\r\n", vt->window.ws_col, vt->window.ws_row);

    size_t new_size = vt->window.ws_col * vt->window.ws_row;
    if (new_size == orig_size) return;

    vt->cells = calloc(vt->window.ws_col * vt->window.ws_row, sizeof(*vt->cells));
    if (!vt->cells) {
        if (original) free(original);
        return;
    }

    if (original) {
        size_t min_size = orig_size > new_size ? new_size : orig_size;
        memcpy(vt->cells, original, min_size * sizeof(*vt->cells));
        free(original);

        size_t cursor = (vt->cursor.y - 1) * original_size.ws_col + vt->cursor.x - 1;

        vt->cursor.x = cursor % vt->window.ws_col + 1;
        vt->cursor.y = cursor / vt->window.ws_col + 1;

        if (vt->cursor.wrap_pending) {
            if (vt->cursor.x != vt->window.ws_col) {
                vt->cursor.x ++;
            }
        }
        vt->cursor.wrap_pending = vt->cursor.x == vt->window.ws_col;
    }

    if (!(vt->cursor.x && vt->cursor.y)) {
        vt->cursor.x = 1;
        vt->cursor.y = 1;
    }

    vt->dirty = true;
    vt_draw_window(vt);
}

#define _signalfd(...) __signalfd(-1, ##__VA_ARGS__, NULL)
int __signalfd(int fd, ...)
{
    va_list va;
    sigset_t mask;
    sigemptyset(&mask);
    va_start(va, fd);
    while (true) {
        int signo = va_arg(va, int);
        if (!signo) break;
        sigaddset(&mask, signo);
    }
    va_end(va);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        return -1;

    return signalfd(fd, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

#define _select(...) __select(0, ##__VA_ARGS__, -1)
int __select(int dummy, ...)
{
    va_list va;
    int nfds = 0;
    fd_set readables;
    FD_ZERO(&readables);
    va_start(va, dummy);
    while (true) {
        int fd = va_arg(va, int);
        if (fd == -1) break;
        if (fd >= nfds) nfds = fd + 1;
        FD_SET(fd, &readables);
    }
    va_end(va);

    int sel = select(nfds, &readables, NULL, NULL, NULL);
    while (sel == -1 && errno == EINTR) {
        sel = select(nfds, &readables, NULL, NULL, NULL);
    }
    int ret = -1;
    if (sel) {
        va_start(va, dummy);
        while (true) {
            int fd = va_arg(va, int);
            if (fd == -1) break;
            if (FD_ISSET(fd, &readables)) {
                ret = fd;
                break;
            }
        }
        va_end(va);
    }
    return ret;
}

int handle_signal(vt *vt, int signo)
{
    switch (signo) {
        case SIGWINCH:
            vt_resize_window(vt);
            return -1;

        case SIGINT:
            fprintf(vt->tty, "^C");
            fflush(vt->tty);
            vt_resize_window(vt);
            return 130;
    }
    UNREACHABLE("Unhandled signal %d", signo);
}

int vt_main_loop(vt *vt)
{
    if (!vt) return 1;

    int sigfd = _signalfd(SIGWINCH, SIGINT);
    bool stdin_eof = false;
    bool stdout_eof = false;
    while (true) {
       int fd;
       if (vt->child_pid) {
          if (stdin_eof && stdout_eof) break;

          int wstatus;
          pid_t waited = waitpid(vt->child_pid, &wstatus, WNOHANG);
          if (waited == -1) {
             perror("waitpid(child, NOHANG)");
             return 1;
          } else if (waited) {
             if (WIFEXITED(wstatus)) {
                int ret = WEXITSTATUS(wstatus);
                fprintf(stderr, "child exited with code %d\n", ret);
             } else if (WIFSIGNALED(wstatus)) {
                int sig = WTERMSIG(wstatus);
                fprintf(stderr, "child was terminated by signal %d (%s)\n", sig, strsignal(sig));
             }
             stdin_eof = true;
             vt->child_pid = 0;
          }

          if (stdin_eof && stdout_eof) break;

          if (stdin_eof) {
             fd = _select(sigfd, vt->stdout[0]);
          } else if (stdout_eof) {
             fd = _select(sigfd, STDIN_FILENO);
          } else {
             fd = _select(sigfd, STDIN_FILENO, vt->stdout[0]);
          }
          fprintf(stderr, "fd = %d\n", fd);
       } else {
          if (stdin_eof) break;

          fd = _select(sigfd, STDIN_FILENO);
       }
       if (fd == sigfd) {
          struct signalfd_siginfo siginfo;
          ssize_t red = read(sigfd, &siginfo, sizeof(siginfo));
          if (red != sizeof(siginfo)) {
             perror("read(sigfd)");
             return 1;
          }

          int ret = handle_signal(vt, siginfo.ssi_signo);
          if (ret != -1) return ret;
       } else {
          uint8_t buf[512];
          ssize_t red = read(fd, buf, sizeof(buf));
          fprintf(stderr, "red from fd %d = %ld, |%.*s| (%02x)\n", fd, red, (int)red, buf, *buf);
          if (red == -1) {
             perror("read()");
             return 1;
          } else if (red == 0) {
             fprintf(stderr, "got eof on fd %d\n", fd);
             if (fd == STDIN_FILENO) {
                stdin_eof = true;
             } else if (fd == vt->stdout[0]) {
                stdout_eof = true;
             } else break;
          }
          if (vt->child_pid) {
             if (fd == STDIN_FILENO) {
                size_t written = 0;
                while (written != (unsigned)red) {
                   ssize_t writ = write(vt->stdin[1], buf + written, red - written);
                   if (writ == -1) {
                      perror("write(child stdin)");
                      return 1;
                   }
                   written += writ;
                   if (written > (unsigned)red) UNREACHABLE("kernel wrote too much");
                }

                /* FIXME have another vt model which can be used to determine pressed keys */
             } else {
                for (size_t i = 0; i < (unsigned)red; i ++) {
                   fprintf(stderr, "vt_process(.., 0x%02X)\n", buf[i]);
                   vt_process(vt, buf[i]);
                   if (vt->dirty) vt_draw_window(vt);
                   fprintf(stderr, "state now %s\n", VT_STATE_STRING(vt->state));
                }
             }
          } else {
             for (size_t i = 0; i < (unsigned)red; i ++) {
                fprintf(stderr, "vt_process(.., 0x%02X)\n", buf[i]);
                vt_process(vt, buf[i]);
                if (vt->dirty) vt_draw_window(vt);
                fprintf(stderr, "state now %s\n", VT_STATE_STRING(vt->state));
             }
          }
       }
    }

    fprintf(stderr, "EOF\n");
    return 0;
}

void vt_rebuild_if_source_newer(const char *program, char * const *argv)
{
    struct stat prog_stat;
    struct stat file_stat;
    /* FIXME this won't work if program is called via PATH */
    if (stat(program, &prog_stat) == -1) return;
    if (stat(__FILE__, &file_stat) == -1) return;
    if (prog_stat.st_mtim.tv_sec < file_stat.st_mtim.tv_sec ||
            (prog_stat.st_mtim.tv_sec == file_stat.st_mtim.tv_sec &&
             prog_stat.st_mtim.tv_nsec < file_stat.st_mtim.tv_nsec)) {
        const char *cc = "cc";
        const char *cflags = "-Wall -Werror -Wextra -Wpedantic -fsanitize=address -ggdb";
        int len = strlen(cc) + 1 + strlen(cflags) + 1 + strlen(__FILE__) + 4 + strlen(program) + 1;
        char *str = malloc(len);
        if (!str) return;
        if (snprintf(str, len, "%s %s %s -o %s", cc, cflags, __FILE__, program) != len - 1) {
            free(str);
            return;
        }
        int ret = system(str);
        free(str);
        if (ret) exit(ret);
        fprintf(stderr, "Reloading as newer source\n");
        execvp(program, argv);
    }
}

int main(int argc, char * const *argv)
{
    vt_rebuild_if_source_newer(*argv, argv);

    printf("argc = %d\n", argc);
#define NEXT_ARG (*(argv++)) + 0 * (argc--)
    __attribute__((unused)) const char *program = NEXT_ARG;
    vt vt = {0};

    /* FIXME process args */

    if (argc) {
       if (pipe2(vt.stdin, O_NONBLOCK) != 0) {
          perror("pipe2(stdin)");
          return 1;
       }

       if (pipe2(vt.stdout, O_NONBLOCK) != 0) {
          perror("pipe2(stdout)");
          return 1;
       }

       vt.child_pid = fork();

       if (vt.child_pid == -1) {
          perror("fork()");
          return 1;
       }

       if (!vt.child_pid) {
          if (dup2(vt.stdin[0], STDIN_FILENO) == -1) {
             perror("dup2(stdin)");
             exit(1);
          }

          if (dup2(vt.stdout[1], STDOUT_FILENO) == -1) {
             perror("dup2(stdout)");
             exit(1);
          }

          close(vt.stdout[0]);
          close(vt.stdin[1]);

          int ret = execvp(*argv, argv);
          if (ret == -1) {
             perror("execvp");
          }
          exit(1);
       }

       close(vt.stdout[1]);
       close(vt.stdin[0]);
    }

    vt_setup_io(&vt);
    vt_resize_window(&vt);
    int ret = vt_main_loop(&vt);
    if (vt.child_pid) {
       close(vt.stdin[1]);
       close(vt.stdout[0]);
       int wstatus;
       if (waitpid(vt.child_pid, &wstatus, 0) != vt.child_pid) {
          perror("waitpid(child)");
          return 1;
       }
       vt.child_pid = 0;
    }
    vt_restore_io(&vt);
    if (vt.cells) {
        free(vt.cells);
        vt.cells = NULL;
    }
    return ret;
#undef NEXT_ARG
}
