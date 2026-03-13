#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
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

#define VT_KEYS_LIST \
   X(VT_KEY_NONE) \
   X(VT_KEY_REQUEST) \
   X(VT_KEY_RAW) \
   X(VT_KEY_ESCAPE) \
   X(VT_KEY_F1) \
   X(VT_KEY_F2) \
   X(VT_KEY_F3) \
   X(VT_KEY_F4) \
   X(VT_KEY_F5) \
   X(VT_KEY_F6) \
   X(VT_KEY_F7) \
   X(VT_KEY_F8) \
   X(VT_KEY_F9) \
   X(VT_KEY_F10) \
   X(VT_KEY_F11) \
   X(VT_KEY_F12) \
   X(VT_KEY_HOME) \
   X(VT_KEY_END) \
   X(VT_KEY_INSERT) \
   X(VT_KEY_DELETE) \
   X(VT_KEY_BACKSPACE) \
   X(VT_KEY_TAB) \
   X(VT_KEY_ENTER) \
   X(VT_KEY_PAGE_UP) \
   X(VT_KEY_PAGE_DOWN) \
   X(VT_KEY_UP) \
   X(VT_KEY_RIGHT) \
   X(VT_KEY_DOWN) \
   X(VT_KEY_LEFT)

#define VT_MODIFIERS_LIST \
   X(VT_MODIFIER_NONE)    E(0) \
   X(VT_MODIFIER_ALT)     E(1 << 0) \
   X(VT_MODIFIER_CONTROL) E(1 << 1) \
   X(VT_MODIFIER_SHIFT)   E(1 << 2) /* Only used for certain special chars, not used for uppercase */ \
   X(VT_MODIFIER_SUPER)   E(1 << 3) /* Only used for certain special chars */

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
   C(0x00) X(VT_CONTROL_NULL)  K(VT_KEY_NONE)      L("Null") S(UNIMPL("VT_CONTROL_NULL")) \
   C(0x03) X(VT_CONTROL_ETX)   K(VT_KEY_NONE)      L("End of Text") S(kill(getpid(), SIGINT)) \
   C(0x05) X(VT_CONTROL_ENQ)   K(VT_KEY_NONE)      L("Enquire") S(UNIMPL("VT_CONTROL_ENQ")) \
   C(0x07) X(VT_CONTROL_BEL)   K(VT_KEY_NONE)      L("Bell") S(_vt_bell(vt)) \
   C(0x08) X(VT_CONTROL_BS)    K(VT_KEY_NONE)      L("Backspace") S(_vt_backspace(vt)) \
   C(0x09) X(VT_CONTROL_HT)    K(VT_KEY_TAB)       L("Horizontal Tab") S(UNIMPL("VT_CONTROL_HT")) \
   C(0x0A) X(VT_CONTROL_LF)    K(VT_KEY_NONE)      L("Line Feed") S(_vt_line_feed(vt)) \
   C(0x0B) X(VT_CONTROL_VT)    K(VT_KEY_NONE)      L("Vertical Tab") S(UNIMPL("VT_CONTROL_VT")) \
   C(0x0C) X(VT_CONTROL_FF)    K(VT_KEY_NONE)      L("Form Feed") S(UNIMPL("VT_CONTROL_FF")) \
   C(0x0D) X(VT_CONTROL_CR)    K(VT_KEY_ENTER)     L("Carriage Return") S(_vt_carriage_return(vt)) \
   C(0x0E) X(VT_CONTROL_SO)    K(VT_KEY_NONE)      L("Shift Out") S(UNIMPL("VT_CONTROL_SO")) \
   C(0x0F) X(VT_CONTROL_SI)    K(VT_KEY_NONE)      L("Shift Out") S(UNIMPL("VT_CONTROL_SI")) \
   C(0x11) X(VT_CONTROL_DC1)   K(VT_KEY_NONE)      L("Device Control 1 (XON)") S(UNIMPL("VT_CONTROL_DC1")) \
   C(0x13) X(VT_CONTROL_DC3)   K(VT_KEY_NONE)      L("Device Control 3 (XOFF)") S(UNIMPL("VT_CONTROL_DC3")) \
   C(0x18) X(VT_CONTROL_CAN)   K(VT_KEY_NONE)      L("Cancel") S(UNIMPL("VT_CONTROL_CAN")) \
   C(0x1A) X(VT_CONTROL_SUB)   K(VT_KEY_NONE)      L("Substitute") S(UNIMPL("VT_CONTROL_SUB")) \
   C(0x1B) X(VT_CONTROL_ESC)   K(VT_KEY_ESCAPE)    L("Escape") S(UNIMPL("VT_CONTROL_ESC")) /* UNREACHABLE, mapped in "anywhere" transitions */ \
   C(0x1C) X(VT_CONTROL_GS)    K(VT_KEY_NONE)      L("Group Separator") S(UNIMPL("VT_CONTROL_GS")) \
   C(0x7F) X(VT_CONTROL_DEL)   K(VT_KEY_BACKSPACE) L("Delete") S(UNIMPL("VT_CONTROL_DEL")) /* UNREACHABLE, used only in VT_ACTION_PRINT or VT_ACTION_IGNORE not VT_ACTION_EXECUTE */ \
   C(0x84) X(VT_CONTROL_IND)   K(VT_KEY_NONE)      L("Index") S(UNIMPL("VT_CONTROL_IND")) \
   C(0x85) X(VT_CONTROL_NEL)   K(VT_KEY_NONE)      L("Next Line") S(UNIMPL("VT_CONTROL_NEL")) \
   C(0x88) X(VT_CONTROL_HTS)   K(VT_KEY_NONE)      L("Horizontal Tab Set") S(UNIMPL("VT_CONTROL_HTS")) \
   C(0x8D) X(VT_CONTROL_RI)    K(VT_KEY_NONE)      L("Reverse Index") S(UNIMPL("VT_CONTROL_RI")) \
   C(0x8E) X(VT_CONTROL_SS2)   K(VT_KEY_NONE)      L("Single shift 2") S(vt->sequence_state.shift = 2; vt->sequence_state.shift_lock = false) \
   C(0x8F) X(VT_CONTROL_SS3)   K(VT_KEY_NONE)      L("Single shift 3") S(vt->sequence_state.shift = 3; vt->sequence_state.shift_lock = false) \
   C(0x90) X(VT_CONTROL_DCS)   K(VT_KEY_NONE)      L("Device Control String") S(UNIMPL("VT_CONTROL_DCS")) /* UNREACHABLE, mapping in "anywhere" transitions */ \
   C(0x98) X(VT_CONTROL_SOS)   K(VT_KEY_NONE)      L("Start Of String") S(UNIMPL("VT_CONTROL_SOS")) \
   C(0x9A) X(VT_CONTROL_DECID) K(VT_KEY_NONE)      L("DEC Private Identification") S(UNIMPL("VT_CONTROL_DECID")) \
   C(0x9B) X(VT_CONTROL_CSI)   K(VT_KEY_NONE)      L("Control Sequence Introducer") S(UNIMPL("VT_CONTROL_CSI")) /* UNREACHABLE, mapped in "anywhere" transitions */ \
   C(0x9C) X(VT_CONTROL_ST)    K(VT_KEY_NONE)      L("String Terminator") S(UNIMPL("VT_CONTROL_ST")) \
   C(0x9D) X(VT_CONTROL_OSC)   K(VT_KEY_NONE)      L("Operating System Command") S(UNIMPL("VT_CONTROL_OSC")) /* UNREACHABLE, mapped in "anywhere" transitions */ \
   C(0x9E) X(VT_CONTROL_PM)    K(VT_KEY_NONE)      L("Privacy Message") S(UNIMPL("VT_CONTROL_PM")) \
   C(0x9F) X(VT_CONTROL_APC)   K(VT_KEY_NONE)      L("Application Program Command") S(UNIMPL("VT_CONTROL_APC"))

#define VT_PARAM(vt, idx, def) ((idx) < (vt)->sequence_state.num_params && (vt)->sequence_state.params[(idx)].non_default ? (vt)->sequence_state.params[(idx)].value : (def))

#define VT_ED "\033[%dJ"
#define VT_CUP "\033[%d;%dH"

#define VT_CSI_FUNCTIONS_LIST \
   C(0x00)         X(VT_CSI_NONE)          K(VT_KEY_NONE)  L("NONE")              S(UNREACHABLE("Unexpected CSI function")) \
   C(0x41 /* A */) X(VT_CSI_CUU)           K(VT_KEY_UP)    L("Cursor Up")         S(_vt_move_cursor_offset(vt, 0, -VT_PARAM(vt, 0, 1))) \
   C(0x42 /* B */) X(VT_CSI_CUD)           K(VT_KEY_DOWN)  L("Cursor Down")       S(_vt_move_cursor_offset(vt, 0, VT_PARAM(vt, 0, 1))) \
   C(0x43 /* C */) X(VT_CSI_CUF)           K(VT_KEY_RIGHT) L("Cursor Forward")    S(_vt_move_cursor_offset(vt, VT_PARAM(vt, 0, 1), 0)) \
   C(0x44 /* D */) X(VT_CSI_CUB)           K(VT_KEY_LEFT)  L("Cursor Backward")   S(_vt_move_cursor_offset(vt, -VT_PARAM(vt, 0, 1), 0)) \
   C(0x48 /* H */) X(VT_CSI_CUP)           K(VT_KEY_NONE)  L("Cursor Position")   S(_vt_move_cursor(vt, VT_PARAM(vt, 1, 1), VT_PARAM(vt, 0, 1))) \
   C(0x4A /* J */) X(VT_CSI_ED)            K(VT_KEY_NONE)  L("Erase In Page")     S(_vt_erase_in_page(vt, VT_PARAM(vt, 0, 0))) \
   C(0x4B /* K */) X(VT_CSI_EL)            K(VT_KEY_NONE)  L("Erase In Line")     S(_vt_erase_in_line(vt, VT_PARAM(vt, 0, 0))) \
   C(0x7E /* ~ */) X(VT_CSI_PRIVATE_TILDE) K(VT_KEY_NONE)  L("CSI Private Tilde") S(_vt_csi_private_tilde_dispatch(vt, VT_PARAM(vt, 0, 0)))

#define VT_CSI_PRIVATE_TILDE_FUNCTIONS_LIST \
   C(0x00) X(VT_CSI_PRIVATE_TILDE_NONE)   K(VT_KEY_NONE)   L("NONE")   S(UNREACHABLE("Unexpected CSI private tilde function")) \
   C(1)    X(VT_CSI_PRIVATE_TILDE_HOME)   K(VT_KEY_HOME)   L("Home")   S(UNIMPL("VT_CSI_PRIVATE_TILDE_HOME")) \
   C(2)    X(VT_CSI_PRIVATE_TILDE_INSERT) K(VT_KEY_INSERT) L("Insert") S(UNIMPL("VT_CSI_PRIVATE_TILDE_INSERT")) \
   C(3)    X(VT_CSI_PRIVATE_TILDE_DELETE) K(VT_KEY_DELETE) L("Delete") S(UNIMPL("VT_CSI_PRIVATE_TILDE_DELETE")) \
   C(4)    X(VT_CSI_PRIVATE_TILDE_END)    K(VT_KEY_END)    L("End")    S(UNIMPL("VT_CSI_PRIVATE_TILDE_END"))

#define VT_CSI_PRIVATE_QUESTION_FUNCTIONS_LIST \
   C(0x00) X(VT_CSI_PRIVATE_QUESTION_NONE)        L("NONE")                      S(UNREACHABLE("Unexpected CSI private question function")) \
   C(25)   X(VT_CSI_PRIVATE_QUESTION_DECTCEM)     L("Show Cursor")               S(fprintf(vt->tty, "\033[?25%c", input); fflush(vt->tty)) \
   C(47)   X(VT_CSI_PRIVATE_QUESTION_ALTBUF)      L("Alternative Screen Buffer") S(_vt_alternate_buffer(vt, input)) \
   C(1000) X(VT_CSI_PRIVATE_QUESTION_VT200_MOUSE) L("VT200 Mouse Reporting")     S(UNIMPL("VT_CSI_PRIVATE_VT200_MOUSE"))

#define C(code)
#define S(code)
#define L(code)
#define K(code)

#define X(name) name
#define E(code) = code, 
typedef enum { VT_MODIFIERS_LIST VT_NUM_MODIFIERS } vt_modifier;

#undef E
#undef X
#define E(code)
#define X(name) name,
typedef enum { VT_KEYS_LIST VT_NUM_KEYS } vt_key;
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
static const char *vt_escape_function_strings[] = { VT_ESCAPE_FUNCTIONS_LIST };
static const char *vt_control_function_strings[] = { VT_CONTROL_FUNCTIONS_LIST };
static const char *vt_csi_function_strings[] = { VT_CSI_FUNCTIONS_LIST };
static const char *vt_csi_private_question_function_strings[] = { VT_CSI_PRIVATE_QUESTION_FUNCTIONS_LIST };
static const char *vt_csi_private_tilde_function_strings[] = { VT_CSI_PRIVATE_TILDE_FUNCTIONS_LIST };
__attribute__((unused)) static const char *vt_action_strings[] = { VT_ACTIONS_LIST };
__attribute__((unused)) static const char *vt_key_strings[] = { VT_KEYS_LIST };
__attribute__((unused)) static const char *vt_modifier_strings[] = { VT_MODIFIERS_LIST };

#define VT_STATE_STRING(stat) (((stat) >= 0 && (stat) < VT_NUM_STATES) ? vt_state_strings[(stat)] : "(state out of bounds)")
#define VT_ACTION_STRING(act) (((act) >= 0 && (act) < VT_NUM_ACTIONS) ? vt_action_strings[(act)] : "(action out of bounds)")
#define VT_ESCAPE_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_ESCAPE_FUNCTIONS) ? vt_escape_function_strings[(func)] : "(escape_function out of bounds)")
#define VT_CONTROL_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_CONTROL_FUNCTIONS) ? vt_control_function_strings[(func)] : "(control_function out of bounds)")
#define VT_CSI_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_CSI_FUNCTIONS) ? vt_csi_function_strings[(func)] : "(csi_function out of bounds)")
#define VT_CSI_PRIVATE_QUESTION_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_CSI_PRIVATE_QUESTION_FUNCTIONS) ? vt_csi_private_question_function_strings[(func)] : "(csi_private_question_function out of bounds)")
#define VT_CSI_PRIVATE_TILDE_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_CSI_PRIVATE_TILDE_FUNCTIONS) ? vt_csi_private_tilde_function_strings[(func)] : "(csi_private_tilde_function out of bounds)")
#define VT_KEY_STRING(func) (((func) >= 0 && (func) < VT_NUM_KEYS) ? vt_key_strings[(func)] : "(key out of bounds)")
#define VT_MODIFIER_STRING(func) (((func) >= 0 && (func) < VT_NUM_MODIFIERS) ? vt_modifier_strings[(func)] : "(modifier out of bounds)")
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
   VT_SCROLL_UP,
   VT_SCROLL_DOWN,
   VT_SCROLL_LEFT,
   VT_SCROLL_RIGHT,
} vt_scroll;

typedef enum
{
    VT_ATTRIBUTE_NONE
} vt_attribute;

typedef struct
{
    bool used;
    char c;
    vt_attribute attribute;
    bool cr;
    bool lf;
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
    size_t shift;
    bool shift_lock;
} vt_sequence_state;

typedef struct
{
   vt_key key;
   vt_modifier modifier;
   char raw;
} vt_key_modifier;

int vt_fprint_key_modifier(FILE *stream, vt_key_modifier key)
{
   if (key.key == VT_KEY_NONE) return 0;

#define print(fmt, ...) do { int _r = fprintf(stream, fmt, ##__VA_ARGS__); if (_r == -1) return -1; ret += _r; } while (false)
   int ret = 0;
   if (key.modifier & VT_MODIFIER_ALT) print("M-");
   if (key.modifier & VT_MODIFIER_CONTROL) print("C-");
   if (key.modifier & VT_MODIFIER_SHIFT) print("S-");
   if (key.modifier & VT_MODIFIER_SUPER) print("G-");

   switch (key.key) {
      case VT_KEY_RAW: print("%c", key.raw); break;
      case VT_KEY_ESCAPE: print("Esc"); break;
      case VT_KEY_F1: print("F1"); break;
      case VT_KEY_F2: print("F2"); break;
      case VT_KEY_F3: print("F3"); break;
      case VT_KEY_F4: print("F4"); break;
      case VT_KEY_F5: print("F5"); break;
      case VT_KEY_F6: print("F6"); break;
      case VT_KEY_F7: print("F7"); break;
      case VT_KEY_F8: print("F8"); break;
      case VT_KEY_F9: print("F9"); break;
      case VT_KEY_F10: print("F10"); break;
      case VT_KEY_F11: print("F11"); break;
      case VT_KEY_F12: print("F12"); break;
      case VT_KEY_HOME: print("Home"); break;
      case VT_KEY_END: print("End"); break;
      case VT_KEY_INSERT: print("Insert"); break;
      case VT_KEY_DELETE: print("Delete"); break;
      case VT_KEY_BACKSPACE: print("Backspace"); break;
      case VT_KEY_TAB: print("Tab"); break;
      case VT_KEY_ENTER: print("Enter"); break;
      case VT_KEY_PAGE_UP: print("Page Up"); break;
      case VT_KEY_PAGE_DOWN: print("Page Down"); break;
      case VT_KEY_UP: print("Up"); break;
      case VT_KEY_RIGHT: print("Right"); break;
      case VT_KEY_DOWN: print("Down"); break;
      case VT_KEY_LEFT: print("Left"); break;

      case VT_KEY_NONE:
      default: UNREACHABLE("Unexpected key %d", key.key);
   }

   return ret;
#undef print
}

typedef struct
{
    vt_state state;
    struct termios original_ios;
    struct termios child_ios;
    bool raw;
    bool nonblocking;
    FILE *tty;
    struct winsize outer_window;
    vt_buffer primary_buffer;
    vt_buffer *alternate_buffer;
    vt_sequence_state sequence_state;
    pid_t child_pid;
    int stdout[2];
    int stderr[2];
    int child_tty;
    vt_key_modifier emitted_key;
} vt;

void vt_process(vt *vt, uint8_t input);

void vt_free(vt *vt)
{
    if (vt->primary_buffer.cells) {
        free(vt->primary_buffer.cells);
        vt->primary_buffer.cells = NULL;
        vt->primary_buffer.width = 0;
        vt->primary_buffer.height = 0;
    }
    if (vt->primary_buffer.saved_cursor) {
       free(vt->primary_buffer.saved_cursor);
       vt->primary_buffer.saved_cursor = NULL;
    }
    if (vt->alternate_buffer) {
       if (vt->alternate_buffer->cells) {
          free(vt->alternate_buffer->cells);
          vt->alternate_buffer->cells = NULL;
          vt->alternate_buffer->width = 0;
          vt->alternate_buffer->height = 0;
       }
       if (vt->alternate_buffer->saved_cursor) {
          free(vt->alternate_buffer->saved_cursor);
          vt->alternate_buffer->saved_cursor = NULL;
       }
       free(vt->alternate_buffer);
       vt->alternate_buffer = NULL;
    }
    vt->primary_buffer.cursor.x = 1;
    vt->primary_buffer.cursor.y = 1;
}

void _vt_scroll(vt *vt, vt_scroll scroll)
{
    if (!vt) return;
    if (vt->emitted_key.key == VT_KEY_REQUEST) return;
    vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
    if (!buffer->cells) return;

    switch (scroll) {
        case VT_SCROLL_UP:
            if (buffer->cursor.y <= 1) return;
            for (size_t row = 1; row < buffer->height; row ++) {
                memcpy(buffer->cells + (row - 1) * buffer->width, buffer->cells + row * buffer->width, buffer->width * sizeof(*buffer->cells));
            }
            memset(buffer->cells + (buffer->height - 1) * buffer->width, '\0', buffer->width * sizeof(*buffer->cells));
            buffer->cursor.y --;
            break;

        case VT_SCROLL_DOWN:
            if (buffer->cursor.y >= buffer->height) return;

            for (size_t row = buffer->height - 2; row >= 1; row --) {
                memcpy(buffer->cells + (row + 1) * buffer->width, buffer->cells + row * buffer->width, buffer->width);
            }
            memset(buffer->cells, '\0', buffer->width);
            buffer->cursor.y ++;
            break;

        case VT_SCROLL_LEFT:
            if (buffer->cursor.x <= 1) return;
            UNIMPL("this will need something diff than memcpy");
            break;

        case VT_SCROLL_RIGHT:
            if (buffer->cursor.x >= buffer->width) return;
            UNIMPL("this will need something diff than memcpy");
            break;

        default: UNREACHABLE("Unexpected scroll value %d", scroll);
    }
}

#define GUTTER_LEFT 3
#define GUTTER_TOP 3

void vt_draw_window(vt *vt)
{
#define CLEAR_SCREEN do { if (fprintf(vt->tty, VT_ED, 2) < 4) { perror("fprintf(CLEAR_SCREEN)"); fprintf(stderr, "Could not write CLEAR_SCREEN fully\n"); } } while (false)
#define GOTO(x, y) do { if (fprintf(vt->tty, VT_CUP, (int)(y), (int)(x)) < 6) { perror("fprintf(GOTO)"); fprintf(stderr, "Could not write GOTO fully\n"); } } while (false)

    if (!vt) return;
    vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
    if (!buffer->cells) return;

    if (!buffer->dirty) return;
    /* fprintf(stderr, "redraw\n"); */

    CLEAR_SCREEN;
    for (size_t x = 10; x <= buffer->width; x += 10) {
        GOTO(GUTTER_LEFT + x, 1);
        if (fputc("0123456789"[(x / 10) % 10], vt->tty) == EOF) {
            perror("fputc()");
            fprintf(stderr, "Couldn't write char for top gutter row 1\n");
        }
        /* fprintf(vt->tty, "%d", (x / 10) % 10); */
    }
    GOTO(GUTTER_LEFT + 1, GUTTER_TOP - 1);
    for (size_t y = 1; y <= buffer->width; y ++) {
        if (fputc("0123456789"[y % 10], vt->tty) == EOF) {
            perror("fputc()");
            fprintf(stderr, "Couldn't write char for top gutter row 2\n");
        }
        /* fprintf(vt->tty, "%d", y % 10); */
    }
    GOTO(GUTTER_LEFT, GUTTER_TOP);
    if (fputc('+', vt->tty) == EOF) {
        perror("fputc()");
        fprintf(stderr, "Couldn't write char for gutter corner\n");
    }
    /* fprintf(vt->tty, "+"); */
    for (size_t x = 1; x <= buffer->width; x ++) {
        if (fputc('-', vt->tty) == EOF) {
            perror("fputc()");
            fprintf(stderr, "Couldn't write char for top gutter border\n");
        }
        /* fprintf(vt->tty, "-"); */
    }
    for (size_t y = 1; y <= buffer->height; y ++) {
        GOTO(1, GUTTER_TOP + y);
        int mod = y % 10;
        if (mod) {
            int writ = fprintf(vt->tty, " %d|", mod);
            if (writ != 3) {
                perror("fprintf()");
                fprintf(stderr, "Could not write single digit left gutter\n");
            }
        } else {
            int writ = fprintf(vt->tty, "%02ld|", y % 100);
            if (writ != 3) {
                perror("fprintf()");
                fprintf(stderr, "Could not write double digit left gutter\n");
            }
        }
    }

    vt_attribute last_attribute = VT_ATTRIBUTE_NONE;

    for (size_t y = 1; y <= buffer->height; y ++) {
        for (size_t x = 1; x <= buffer->width; x ++) {
            vt_cell *cell = &buffer->cells[(y - 1) * buffer->width + x - 1];
            if (cell->used) {
                if (cell->attribute != last_attribute) {
                    UNIMPL("set attribute");
                }
                if (x == 1 || !buffer->cells[(y - 1) * buffer->width + x - 2].used) {
                    GOTO(x + GUTTER_LEFT, y + GUTTER_TOP);
                    /* fprintf(stderr, "cell '%c'\n", cell->c); */
                }
                if (fputc(cell->c, vt->tty) == EOF) {
                    perror("fputc()");
                    fprintf(stderr, "Could not write char in grid\n");
                }
                /* fprintf(vt->tty, "%c", cell->c); */
            }
        }
    }

    GOTO(buffer->cursor.x + GUTTER_LEFT, buffer->cursor.y + GUTTER_TOP);

    while (true) {
       int flush = fflush(vt->tty);
       if (flush == EOF) {
          int e = errno;
           perror("fflush(tty)");
           if (e == EAGAIN) continue;
       }
       break;
    }
    buffer->dirty = false;
#undef GOTO
}

int vt_fprintc(FILE *stream, char input)
{
    int first = fprintf(stream, "%02X", input);
    if (first == -1) return -1;
    int second = 0;
    switch (input) {
       case '"': second = fprintf(stream, " '\\\"'"); break;
       case '\\': second = fprintf(stream, " '\\\\"); break;
       case '\a': second = fprintf(stream, " '\\a"); break;
       case '\033': second = fprintf(stream, " '\\e'"); break;
       case '\b': second = fprintf(stream, " '\\b"); break;
       case '\f': second = fprintf(stream, " '\\f"); break;
       case '\n': second = fprintf(stream, " '\\n'"); break;
       case '\r': second = fprintf(stream, " '\\r'"); break;
       case '\t': second = fprintf(stream, " '\\t'"); break;
       case '\v': second = fprintf(stream, " '\\v'"); break;
       default: if (isprint(input)) second = fprintf(stream, " '%c'", input);
    }
    return second == -1 ? -1 : first + second;
}

void vt_resize_window(vt *vt)
{
   /* FIXME resize so that the _bottom_ of the buffer is shown */
   /* FIXME could detect if text (every row with data has it from start of row contiguously, end can be blank) */
   // on text do one thing, on "data" try and keep the same coords
   // perhaps try to replay semantics by reusing _vt_print _vt_execute etc
    if (!vt) return;
    vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
    fprintf(stderr, "outer window size was %ux%u\r\n", vt->outer_window.ws_col, vt->outer_window.ws_row);

    vt_cell *original = buffer->cells;
    fprintf(stderr, "outer window size was %ux%u\r\n", vt->outer_window.ws_col, vt->outer_window.ws_row);
    fprintf(stderr, "inner window size was %lux%lu\r\n", buffer->width, buffer->height);
    struct winsize original_size = {.ws_col = buffer->width, .ws_row = buffer->height};
    size_t orig_size = buffer->width * buffer->height;

    vt->outer_window = (struct winsize){0};
    int fds[] = { STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO };
    for (size_t i = 0; i < C_ARRAY_LEN(fds); i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &vt->outer_window) == 0 && vt->outer_window.ws_col && vt->outer_window.ws_row)
            break;
        vt->outer_window = (struct winsize){0};
    }

    fprintf(stderr, "outer window size %ux%u\r\n", vt->outer_window.ws_col, vt->outer_window.ws_row);

    if (vt->outer_window.ws_col > GUTTER_LEFT && vt->outer_window.ws_row > GUTTER_TOP) {
        buffer->width = vt->outer_window.ws_col - GUTTER_LEFT;
        buffer->height = vt->outer_window.ws_row - GUTTER_TOP;
    } else {
        buffer->width = 80;
        buffer->height = 24;
    }

    fprintf(stderr, "inner window size %lux%lu\r\n", buffer->width, buffer->height);

    size_t new_size = buffer->width * buffer->height;
    if (new_size == orig_size) return;

    buffer->cells = calloc(buffer->width * buffer->height, sizeof(*buffer->cells));
    if (!buffer->cells) {
        if (original) free(original);
        return;
    }

    if (original) {
       /* FIXME store and retrieve points where a \r\n was done, then replay that */
       size_t dst_row = 0;
       size_t dst_col = 0;
       vt_cell *src_cursor = &original[(buffer->cursor.y - 1) * original_size.ws_col + buffer->cursor.x - 1];
       vt_cell *dst_cursor = NULL;
       for (size_t src_row = 0; src_row < original_size.ws_row; src_row ++) {
          for (size_t src_col = 0; src_col < original_size.ws_col; src_col ++) {
             if (dst_col == buffer->width) {
                /* fprintf(stderr, "line wrap\n"); */
                dst_col = 0;
                dst_row ++;
             }
             if (dst_row == buffer->height) {
                /* fprintf(stderr, "screen wrap, data lost\n"); */
                vt_cell *src_cell = &original[src_row * original_size.ws_col + src_col];
                vt_cell *dst_cell = &buffer->cells[dst_row * buffer->width + dst_col];

                if (src_cell == src_cursor) dst_cursor = dst_cell;

                bool used = false;
                for (vt_cell *cell = &original[src_row * original_size.ws_col + src_col];
                      cell < original + orig_size; cell ++) {
                   if (cell->used) {
                      used = true;
                      break;
                   }
                }

                if (used) {
                   buffer->dirty = true;
                   vt_draw_window(vt);
                   _vt_scroll(vt, VT_SCROLL_UP);
                   buffer->dirty = true;
                   vt_draw_window(vt);
                   dst_row --;
                } else continue;

             }

             vt_cell *src_cell = &original[src_row * original_size.ws_col + src_col];
             vt_cell *dst_cell = &buffer->cells[dst_row * buffer->width + dst_col];

             if (src_cell == src_cursor) dst_cursor = dst_cell;

             if (src_cell->used) {
                /* fprintf(stderr, "copying from %ldx%ld to %ldx%ld: ", src_col + 1, src_row + 1, dst_col + 1, dst_row + 1); */
                /* vt_fprintc(stderr, src_cell->c); */
                /* fprintf(stderr, "\n"); */
                memcpy(dst_cell, src_cell, sizeof(*dst_cell));

                if (src_cell->cr) {
                   fprintf(stderr, "CR\n");
                   dst_col = 0;
                } else if (src_cell->lf) {
                   for (size_t off = 1; src_col + off < original_size.ws_col; off ++) {
                      if (src_cell + off == src_cursor) dst_cursor = dst_cell + off;
                      if ((src_cell + off)->used) {
                         fprintf(stderr, "copying stuff after \\n from %ldx%ld to %ldx%ld: ",
                               src_col + off + 1, src_row + 1,
                               (dst_col + off) % buffer->width + 1,
                               (dst_col + off) / buffer->width + dst_row + 1);
                         vt_fprintc(stderr, (src_cell + off)->c);
                         fprintf(stderr, "\n");
                         memcpy(dst_cell + off, src_cell + off, sizeof(*dst_cell));
                      }
                   }
                   fprintf(stderr, "LF\n");
                   dst_row ++;
                   if (true /* FIXME add option for \n -> \r\n semantics */) {
                      dst_col = 0;
                   }
                   break; // Go to next line
                } else {
                   dst_col ++;
                }
             } else {
                dst_col ++;
             }
          }
       }
        /* size_t min_size = orig_size > new_size ? new_size : orig_size; */
        /* memcpy(vt->cells, original, min_size * sizeof(*vt->cells)); */
        free(original);

        /* size_t cursor = (vt->cursor.y - 1) * original_size.ws_col + vt->cursor.x - 1; */
        size_t cursor = dst_cursor ? dst_cursor - buffer->cells : 0;
        fprintf(stderr, "moving cursor from %ldx%ld to %ldx%ld\n", 
              buffer->cursor.x, buffer->cursor.y,
              cursor % buffer->width + 1, cursor / buffer->width + 1);
        if (cursor >= new_size) cursor = new_size - 1;

        fprintf(stderr, "moving cursor from %ldx%ld to %ldx%ld\n", 
              buffer->cursor.x, buffer->cursor.y,
              cursor % buffer->width + 1, cursor / buffer->width + 1);
        buffer->cursor.x = cursor % buffer->width + 1;
        buffer->cursor.y = cursor / buffer->width + 1;

        if (buffer->cursor.wrap_pending) {
            if (buffer->cursor.x != buffer->width) {
                buffer->cursor.x ++;
              fprintf(stderr, "moving cursor to %ldx%ld as was marked as wrap pending\n", buffer->cursor.x, buffer->cursor.y);
            }
        }
        buffer->cursor.wrap_pending = buffer->cursor.x == buffer->width;
        if (buffer->cursor.wrap_pending) fprintf(stderr, "wrap pending\n");
    }

    if (!(buffer->cursor.x && buffer->cursor.y)) {
        buffer->cursor.x = 1;
        buffer->cursor.y = 1;
    }

    buffer->dirty = true;
    vt_draw_window(vt);

    /* FIXME resize the primary buffer if on alternate buffer */
    // a way to do this could be to do the resize when turning off alt buffer, but need a way to know the size of the buffer...
    // also whether primary buffer has scrollback to care about?
}

void _vt_bell(vt *vt)
{
   if (!vt) return;

   // Send to controlling tty (may beep, may flash)
   fputc('\a', vt->tty);

   /* FIXME flash our virtual terminal */
   // needs extra param to draw_window to invert, which would apply ontop of any SGR mods
   // then draw inverted, sleep a tiny bit, draw back to normal
   //vt_draw_window(vt);
}

void _vt_backspace(vt *vt)
{
   if (!vt) return;
   if (vt->emitted_key.key == VT_KEY_REQUEST) return;
   vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
   if (buffer->cursor.x > 1) {
      buffer->cursor.x --;
      buffer->dirty = true;
   }
}

void _vt_line_feed(vt *vt)
{
   if (!vt) return;
   if (vt->emitted_key.key == VT_KEY_REQUEST) return;
   vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
   if (!buffer->cells) return;

   if (buffer->cursor.x > 1 || buffer->cursor.y > 1) buffer->cells[(buffer->cursor.y - 1) * buffer->width + buffer->cursor.x - 1 - (buffer->cursor.wrap_pending ? 0 : 1)].lf = true;
   if (buffer->cursor.y == buffer->height) _vt_scroll(vt, VT_SCROLL_UP);
   buffer->cursor.y ++;
   if (true /* FIXME add option for \n -> \r\n semantics */) buffer->cursor.x = 1;
   buffer->dirty = true;
}

void _vt_carriage_return(vt *vt)
{
   if (!vt) return;
   if (vt->emitted_key.key == VT_KEY_REQUEST) return;
   vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
   if (!buffer->cells) return;

   if (buffer->cursor.x > 1 || buffer->cursor.y > 1) buffer->cells[(buffer->cursor.y - 1) * buffer->width + buffer->cursor.x - 1 - (buffer->cursor.wrap_pending ? 0 : 1)].cr = true;
   buffer->cursor.x = 1;
   buffer->dirty = true;
}

void _vt_save_cursor(vt *vt)
{
   if (!vt) return;
   if (vt->emitted_key.key == VT_KEY_REQUEST) return;
   vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
   if (!buffer->cells) return;

   if (!buffer->saved_cursor) {
      buffer->saved_cursor = calloc(1, sizeof(*buffer->saved_cursor));
      if (!buffer->saved_cursor) return;
   }
   *buffer->saved_cursor = buffer->cursor;
}

void _vt_move_cursor_offset(vt *vt, int off_x, int off_y)
{
   if (!vt) return;
   if (vt->emitted_key.key == VT_KEY_REQUEST) return;
   vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
   if (!buffer->cells) return;

   if (off_y) {
      if (off_y < 0 && (unsigned)-off_y >= buffer->cursor.y) {
         buffer->cursor.y = 1;
      } else if (off_y > 0 && buffer->cursor.y + off_y > buffer->height) {
         buffer->cursor.y = buffer->height;
      } else {
         buffer->cursor.y = buffer->cursor.y + off_y;
      }
      buffer->dirty = true;
   }
   if (off_x) {
      if (off_x < 0 && (unsigned)-off_x >= buffer->cursor.x) {
         buffer->cursor.x = 1;
      } else if (off_x > 0 && buffer->cursor.x + off_x > buffer->width) {
         buffer->cursor.x = buffer->width;
      } else {
         buffer->cursor.x = buffer->cursor.x + off_x;
      }
      buffer->dirty = true;
   }

   buffer->cursor.wrap_pending = buffer->cursor.x == buffer->width;
}

void _vt_move_cursor(vt *vt, uint16_t x, uint16_t y)
{
   if (!vt) return;
   if (vt->emitted_key.key == VT_KEY_REQUEST) return;
   vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
   if (!buffer->cells) return;

   if (!x) x = 1;
   if (!y) y = 1;

   buffer->cursor.x = x ? (x <= buffer->width ? x : buffer->width) : 1;
   buffer->cursor.y = y ? (y <= buffer->height ? y : buffer->height) : 1;
   buffer->cursor.wrap_pending = buffer->cursor.x == buffer->width;
   buffer->dirty = true;
}

void _vt_alternate_buffer(vt *vt, uint8_t input)
{
   /* UNIMPL("sizeof(vt_buffer) = %zu", sizeof(vt_buffer)); */
   static_assert(sizeof(vt_buffer) == 64, "State added, may need freeing");
   if (!vt) return;
   if (vt->emitted_key.key == VT_KEY_REQUEST) return;

   switch (input) {
      case 'h':
         if (vt->alternate_buffer) return;
         vt->alternate_buffer = calloc(1, sizeof(*vt->alternate_buffer));
         if (!vt->alternate_buffer) return;
         memcpy(vt->alternate_buffer, &vt->primary_buffer, sizeof(*vt->alternate_buffer));
         vt->alternate_buffer->cells = calloc(vt->alternate_buffer->width * vt->alternate_buffer->height, sizeof(*vt->alternate_buffer->cells));
         if (!vt->alternate_buffer->cells) {
            free(vt->alternate_buffer);
            vt->alternate_buffer = NULL;
            return;
         }

         vt->alternate_buffer->saved_cursor = NULL;
         vt->alternate_buffer->dirty = true;
         break;

      case 'l':
         if (!vt->alternate_buffer) return;
         if (vt->alternate_buffer->cells) {
            free(vt->alternate_buffer->cells);
            vt->alternate_buffer->cells = NULL;
         }
         if (vt->alternate_buffer->saved_cursor) {
            free(vt->alternate_buffer->saved_cursor);
            vt->alternate_buffer->saved_cursor = NULL;
         }
         free(vt->alternate_buffer);
         vt->alternate_buffer = NULL;
         vt->primary_buffer.dirty = true;
         break;

      default: UNREACHABLE("Unexpected CSI terminator for alternative buffer");
   }
}

void _vt_erase_in_line(vt *vt, uint16_t param)
{
   if (!vt) return;
   if (vt->emitted_key.key == VT_KEY_REQUEST) return;
   vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
   if (!buffer->cells) return;

   switch (param) {
      case 0:
              memset(buffer->cells + (buffer->cursor.y - 1) * buffer->width + buffer->cursor.x - 1, '\0', sizeof(*buffer->cells) * (buffer->width - buffer->cursor.x + 1));
                 break;
      case 1:
              memset(buffer->cells + (buffer->cursor.y - 1) * buffer->width, '\0', sizeof(*buffer->cells) * (buffer->cursor.x));
                 break;
      case 2:
              memset(buffer->cells + (buffer->cursor.y - 1) * buffer->width, '\0', sizeof(*buffer->cells) * buffer->width);
                 break;
      default: UNREACHABLE("Unexpected param to ED: %d", param);
   }
   buffer->dirty = true;
}

void _vt_erase_in_page(vt *vt, uint16_t param)
{
   if (!vt) return;
   if (vt->emitted_key.key == VT_KEY_REQUEST) return;
   vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer;
   if (!buffer->cells) return;

   switch (param) {
      case 0:
              memset(buffer->cells + (buffer->cursor.y - 1) * buffer->width + buffer->cursor.x - 1, '\0', sizeof(*buffer->cells) * ((buffer->width * buffer->height) - ((buffer->cursor.y - 1) * buffer->width + buffer->cursor.x - 1)));
                 break;
      case 1:
              memset(buffer->cells, '\0', sizeof(*buffer->cells) * (((buffer->cursor.y - 1) * buffer->width + buffer->cursor.x)));
                 break;
      case 2:
              memset(buffer->cells, '\0', sizeof(*buffer->cells) * (buffer->width * buffer->height));
                 break;
      default: UNREACHABLE("Unexpected param to ED: %d", param);
   }
   buffer->dirty = true;
}

void _vt_print(vt *vt, char input)
{
    if (!vt) return;
    if (vt->emitted_key.key == VT_KEY_REQUEST) {
       switch (vt->sequence_state.shift) {
          case 0:
             switch (input) {
                case 0x7F: vt->emitted_key = (vt_key_modifier){.key = VT_KEY_BACKSPACE}; break;
                default: vt->emitted_key = (vt_key_modifier){.key = VT_KEY_RAW, .raw = input}; break;
             }
             break;

          case 1: UNIMPL("Shift G1"); break;

          case 2: UNIMPL("Shift G2"); break;

          case 3:
             switch (input) {
                case 'P': vt->emitted_key = (vt_key_modifier){.key = VT_KEY_F1}; break;
                case 'Q': vt->emitted_key = (vt_key_modifier){.key = VT_KEY_F2}; break;
                case 'R': vt->emitted_key = (vt_key_modifier){.key = VT_KEY_F3}; break;
                case 'S': vt->emitted_key = (vt_key_modifier){.key = VT_KEY_F4}; break;

                default: UNIMPL("Shift G3"); break;
             }
             break;

          default: UNREACHABLE("Unexpected shift %lu", vt->sequence_state.shift);
       }
       if (vt->sequence_state.shift && !vt->sequence_state.shift_lock) {
          vt->sequence_state.shift = 0;
       }
       if (vt->emitted_key.key != VT_KEY_REQUEST) return;
    }
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
      if (vt->sequence_state.num_params != C_ARRAY_LEN(vt->sequence_state.params)) {
          if (vt->sequence_state.num_params) {
             param = &vt->sequence_state.params[vt->sequence_state.num_params - 1];
             param->value *= 10;
             param->value += input - '0';
          } else {
             param = &vt->sequence_state.params[vt->sequence_state.num_params ++];
             param->value = input - '0';
          }
      }
      if (param) param->non_default = true;
   } else if (vt->sequence_state.num_params != C_ARRAY_LEN(vt->sequence_state.params)) {
      param = &vt->sequence_state.params[vt->sequence_state.num_params ++];
      param->non_default = false;
   }

   if (param) {
      /* fprintf(stderr, "in _vt_param, input %02X (%c), num params %zu, param so far %d (default=%d)\n", input, input, vt->sequence_state.num_params, param->value, !param->non_default); */
   } else {
      /* fprintf(stderr, "in _vt_param, input %02X (%c), num params %zu, ignoring more\n", input, input, vt->sequence_state.num_params); */
   }
}

#undef C
#undef K

void _vt_execute(vt *vt, uint8_t input)
{
    if (!vt) return;

#define S(code)
#define X(name)
#define C(code) case code:
#define K(_k) if (_k != VT_KEY_NONE) vt->emitted_key = (vt_key_modifier){.key = _k}; break;
    if (vt->emitted_key.key == VT_KEY_REQUEST) {
       switch (input) { VT_CONTROL_FUNCTIONS_LIST }
       if (vt->emitted_key.key != VT_KEY_REQUEST) return;

       if (!iscntrl(input)) UNREACHABLE("Unexpected input for execute %02X", input);
       vt->emitted_key = (vt_key_modifier){
          .modifier = VT_MODIFIER_CONTROL,
             .key = VT_KEY_RAW,
             .raw = input + 'a' - 1,
       };
       return;
    }
#undef K
#undef X

#define K(code)
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
#undef K
}

void _vt_escape_dispatch(vt *vt, uint8_t input)
{
    if (!vt) return;

    if (vt->emitted_key.key == VT_KEY_REQUEST) {
       vt->emitted_key = (vt_key_modifier){
          .modifier = VT_MODIFIER_ALT | (iscntrl(input) ? VT_MODIFIER_CONTROL : VT_MODIFIER_NONE),
          .key = VT_KEY_RAW,
          .raw = iscntrl(input) ? input + 'a' - 1 : input,
       };
       return;
    }

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
#define X(name)
#define C(code) case code:
#define K(_k) if (_k != VT_KEY_NONE) vt->emitted_key = (vt_key_modifier){.key = _k}; break;
    if (vt->emitted_key.key == VT_KEY_REQUEST) {
       if (vt->sequence_state.num_params > 1) {
          UNIMPL("modifiers");
       }
       switch (param) { VT_CSI_PRIVATE_TILDE_FUNCTIONS_LIST }
       if (vt->emitted_key.key != VT_KEY_REQUEST) return;
    }
#undef K
#undef X

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
#undef K
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
#define X(name)
#define C(code) case code:
#define K(_k) if (_k != VT_KEY_NONE) vt->emitted_key = (vt_key_modifier){.key = _k}; break;
    if (vt->emitted_key.key == VT_KEY_REQUEST) {
       switch (input) { VT_CSI_FUNCTIONS_LIST }
       if (vt->emitted_key.key != VT_KEY_REQUEST) return;
    }
#undef K
#undef X

#define K(code)
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

    /* A few hardcoded hacks for input keys */
    if (vt->emitted_key.key == VT_KEY_REQUEST) {
       if (vt->state == VT_STATE_ESCAPE) {
          switch (input) {
             case '\033':
                vt->emitted_key = (vt_key_modifier){
                   .key = VT_KEY_ESCAPE,
                   .modifier = VT_MODIFIER_ALT,
                };
                break;

             case '\r':
                vt->emitted_key = (vt_key_modifier){
                   .key = VT_KEY_ENTER,
                   .modifier = VT_MODIFIER_ALT,
                };
                break;
          }
       }
       if (vt->emitted_key.key != VT_KEY_REQUEST) return;
    }

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
    setbuf(vt->tty, NULL);


    if (tcgetattr(STDIN_FILENO, &vt->original_ios) == 0) {
        struct termios new_ios = vt->original_ios;
        cfmakeraw(&new_ios);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &new_ios) == 0) {
            vt->raw = true;
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
        if (tcsetattr(STDOUT_FILENO, TCSANOW, &vt->original_ios) == 0) {
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

#define GOTO(x, y) fprintf(stdout, VT_CUP, (int)(y), (int)(x))
    GOTO(vt->outer_window.ws_col, vt->outer_window.ws_row);
    printf("\n");
    fflush(stdout);
#undef GOTO
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
    fprintf(stderr, "signal %d (%s)\n", signo, strsignal(signo));
    switch (signo) {
        case SIGWINCH:
            vt_resize_window(vt);
            if (vt->child_tty) {
               struct winsize window = {
                  .ws_col = vt->alternate_buffer ? vt->alternate_buffer->width : vt->primary_buffer.width,
                  .ws_row = vt->alternate_buffer ? vt->alternate_buffer->height : vt->primary_buffer.height,
               };
               if (ioctl(vt->child_tty, TIOCSWINSZ, &window) != 0) {
                  perror("ioctl(child_tty, TIOCSWINSZ, &window)");
                  return 1;
               }
               if (kill(vt->child_pid, signo) == -1) {
                  perror("kill(SIGWINCH, child)");
                  return 1;
               }
            }
            return -1;

         case SIGINT:
            fprintf(vt->tty, "^C");
            if (vt->child_tty) {
               if (kill(vt->child_pid, signo) == -1) {
                  perror("kill(SIGINT, child)");
               }
            }
            return 130;

         case SIGCHLD:
         {
            int wstatus;
            pid_t waited = waitpid(vt->child_pid, &wstatus, WNOHANG);
            if (waited == -1) {
               if (errno == ECHILD) {
                  vt->child_pid = 0;
                  break;
               }
               perror("waitpid(child, NOHANG)");
               return 1;
            } else if (waited) {
               int ret;
               if (WIFEXITED(wstatus)) {
                  ret = WEXITSTATUS(wstatus);
                  fprintf(stderr, "child exited with code %d\n", ret);
               } else if (WIFSIGNALED(wstatus)) {
                  int sig = WTERMSIG(wstatus);
                  fprintf(stderr, "child was terminated by signal %d (%s)\n", sig, strsignal(sig));
                  ret = 128 + sig;
               }
               vt->child_pid = 0;
               return ret;
            }
         }; return 0;
    }
    UNREACHABLE("Unhandled signal %d %s", signo, strsignal(signo));
    return 1;
}

int vt_main_loop(vt *vt)
{
    if (!vt) return 1;

    int sigfd = _signalfd(SIGWINCH, SIGINT, vt->child_pid ? SIGCHLD : SIGKILL);
    bool stdin_eof = false;
    bool stdout_eof = false;
    bool stderr_eof = false;
    bool tty_eof = false;

    while (true) {
       int fd;
       if (vt->child_pid) {
          if (stdin_eof && stdout_eof && stderr_eof && tty_eof) break;

          if (stdin_eof && stdout_eof && stderr_eof && tty_eof) break;

          fprintf(stderr, "select(sigfd, stdin, stdout, stderr, tty)\n");
          fd = _select(sigfd, STDIN_FILENO, vt->stdout[0], vt->stderr[0], vt->child_tty);
       } else {
          if (stdin_eof) break;

          fprintf(stderr, "select(sigfd, stdin)\n");
          fd = _select(sigfd, STDIN_FILENO);
       }
       fprintf(stderr, "selected fd %d (%s)\n", fd, (fd == sigfd ? "signal" : (fd == STDIN_FILENO ? "stdin" : (fd == vt->stdout[0] ? "child stdout" : (fd == vt->stderr[0] ? "child stderr" : (fd == vt->child_tty ? "child tty" : "unknown"))))));
       if (fd == sigfd) {
          struct signalfd_siginfo siginfo;
          ssize_t red = read(sigfd, &siginfo, sizeof(siginfo));
          if (red == -1 && errno == EAGAIN) continue;
          if (red != sizeof(siginfo)) {
             if (red == -1) fprintf(stderr, "read(sigfd) = %ld\n", red);
             else perror("read(sigfd)");
             return 1;
          }

          if (siginfo.ssi_signo == SIGCHLD) {
             stdin_eof = true;
             stdout_eof = true;
             stderr_eof = true;
             tty_eof = true;
             close(vt->stdout[0]);
             close(vt->stderr[0]);
             close(vt->child_tty);
          }

          int ret = handle_signal(vt, siginfo.ssi_signo);
          if (ret != -1) return ret;
       } else {
           while (true) {
              uint8_t buf[512] = {0};
              ssize_t red = read(fd, buf, sizeof(buf));

              if (red == -1) {
                  if (errno == EAGAIN) break;
                 perror("read()");
                 return 1;

              } else if (red == 0) {
                 fprintf(stderr, "got eof on fd %d (%s)\n", fd, (fd == STDIN_FILENO ? "stdin" : (fd == vt->stdout[0] ? "child stdout" : (fd == vt->stderr[0] ? "child stderr" : (fd == vt->child_tty ? "child tty" : "unknown")))));
                 close(fd);
                 if (fd == STDIN_FILENO) {
                    stdin_eof = true;
                 } else if (fd == vt->stdout[0]) {
                    stdout_eof = true;
                 } else if (fd == vt->stderr[0]) {
                    stderr_eof = true;
                 } else if (fd == vt->child_tty) {
                    tty_eof = true;
                 } else {
                    fprintf(stderr, "Unexpected fd\n");
                 }
                 break;
              }

              fprintf(stderr, "red %ld from fd %d (%s):", red, fd, (fd == STDIN_FILENO ? "stdin" : (fd == vt->stdout[0] ? "child stdout" : (fd == vt->stderr[0] ? "child stderr" : (fd == vt->child_tty ? "child tty" : "unknown")))));
              bool allgraph = true;
              for (ssize_t i = 0; i < red; i ++) {
                  if (!isgraph(buf[i])) {
                      allgraph = false;
                      break;
                  }
              }
              if (allgraph) {
                 fprintf(stderr, " |%.*s|\n", (int)red, buf);
              } else {
                 fprintf(stderr, " (");
                 for (ssize_t i = 0; i < red; i ++) {
                    if (i) fprintf(stderr, ", ");
                    vt_fprintc(stderr, buf[i]);
                 }
                 fprintf(stderr, ")\n");
              }

              if (vt->child_tty > 0) {
                 if (fd == STDIN_FILENO) {
                    size_t written = 0;
                    while (written != (unsigned)red) {
                       ssize_t writ = write(vt->child_tty, buf + written, red - written);
                       if (writ == -1) {
                          perror("write(child stdin)");
                          return 1;
                       }
                       written += writ;
                       if (written > (unsigned)red) UNREACHABLE("kernel wrote too much");
                    }

                    for (size_t i = 0; i < (unsigned)red; i ++) {
                       if (vt->emitted_key.key != VT_KEY_REQUEST) {
                          vt->emitted_key.key = VT_KEY_REQUEST;
                          vt->state = VT_STATE_GROUND;
                       }
                       /* fprintf(stderr, "vt_process(.., 0x%02X)\n", buf[i]); */
                       vt_process(vt, buf[i]);
                       /* fprintf(stderr, "state now %s\n", VT_STATE_STRING(vt->state)); */
                       /* fprintf(stderr, "cell now %ldx%ld\n", vt->cursor.x, vt->cursor.y); */
                       /* if (vt->alternate_buffer) fprintf(stderr, "using alternate buffer\n"); */
                       if (vt->emitted_key.key != VT_KEY_REQUEST) {
                          fprintf(stderr, "emitted key ");
                          vt_fprint_key_modifier(stderr, vt->emitted_key);
                          fprintf(stderr, "\n");
                          memset(&vt->emitted_key, '\0', sizeof(vt->emitted_key));
                       }
                    }

                    if (vt->emitted_key.key == VT_KEY_REQUEST) {
                       bool match = true;
                       switch (vt->state) {
                          case VT_STATE_ESCAPE:
                             vt->emitted_key = (vt_key_modifier){.key = VT_KEY_ESCAPE};
                             break;

                          case VT_STATE_CSI_ENTRY:
                             vt->emitted_key = (vt_key_modifier){.key = VT_KEY_RAW, .raw = '[', .modifier = VT_MODIFIER_ALT};
                             break;
                          default: match = false;
                       }
                       if (match) {
                          fprintf(stderr, "emitted key ");
                          vt_fprint_key_modifier(stderr, vt->emitted_key);
                          fprintf(stderr, "\n");
                          memset(&vt->emitted_key, '\0', sizeof(vt->emitted_key));
                          vt->emitted_key.key = VT_KEY_REQUEST;
                          _vt_transition(vt, VT_STATE_GROUND, '\0');
                       }
                    } else {
                       _vt_transition(vt, VT_STATE_GROUND, '\0');
                       vt->emitted_key.key = VT_KEY_NONE;
                    }
                    vt_draw_window(vt);
                 } else if (fd == vt->stderr[0] && false /* FIXME add option to send stderr to vt */) {
                    size_t written = 0;
                    while (written != (unsigned)red) {
                       ssize_t writ = write(STDERR_FILENO, buf + written, red - written);
                       if (writ == -1) {
                          perror("write(stderr)");
                          return 1;
                       }
                       written += writ;
                       if (written > (unsigned)red) UNREACHABLE("kernel wrote too much");
                    }

                 } else {
                    for (size_t i = 0; i < (unsigned)red; i ++) {
                       /* fprintf(stderr, "vt_process(.., 0x%02X)\n", buf[i]); */
                       vt_process(vt, buf[i]);
                       /* fprintf(stderr, "state now %s\n", VT_STATE_STRING(vt->state)); */
                       /* fprintf(stderr, "cell now %ldx%ld\n", vt->cursor.x, vt->cursor.y); */
                       /* if (vt->alternate_buffer) fprintf(stderr, "using alternate buffer\n"); */
                    }
                    vt_draw_window(vt);
                 }
              } else {
                 for (size_t i = 0; i < (unsigned)red; i ++) {
                    /* fprintf(stderr, "vt_process(.., 0x%02X)\n", buf[i]); */
                    vt_process(vt, buf[i]);
                    /* fprintf(stderr, "state now %s\n", VT_STATE_STRING(vt->state)); */
                    /* vt_buffer *buffer = vt->alternate_buffer ? vt->alternate_buffer : &vt->primary_buffer; */
                    /* fprintf(stderr, "cell now %ldx%ld\n", buffer->cursor.x, buffer->cursor.y); */
                    /* if (vt->alternate_buffer) fprintf(stderr, "using alternate buffer\n"); */
                 }
                 vt_draw_window(vt);
              }
              break;
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

int vt_setup_child(vt *vt, char * const *argv)
{
   if (pipe2(vt->stdout, O_NONBLOCK) != 0) {
      perror("pipe2(stdout)");
      return 1;
   }

   if (pipe2(vt->stderr, O_NONBLOCK) != 0) {
      perror("pipe2(stderr)");
      return 1;
   }

   vt->child_ios = vt->original_ios;
   struct winsize window = {.ws_col = vt->primary_buffer.width, .ws_row = vt->primary_buffer.height};
   vt->child_pid = forkpty(&vt->child_tty, NULL, &vt->child_ios, &window);

   if (vt->child_pid == -1) {
      perror("fork()");
      return 1;
   }

   if (!vt->child_pid) {
      if (dup2(vt->stdout[1], STDOUT_FILENO) == -1) {
         perror("dup2(stdout)");
         _exit(1);
      }

      if (dup2(vt->stderr[1], STDERR_FILENO) == -1) {
         perror("dup2(stderr)");
         _exit(1);
      }

      close(vt->stderr[0]);
      close(vt->stdout[0]);

      int ret = execvp(*argv, argv);
      if (ret == -1) {
         perror("execvp");
      }
      _exit(1);
   }

   fprintf(stderr, "Parent %d\n", getpid());
   fprintf(stderr, "Started child %d\n", vt->child_pid);

   close(vt->stdout[1]);
   close(vt->stderr[1]);

   return 0;
}

int main(int argc, char * const *argv)
{
    vt_rebuild_if_source_newer(*argv, argv);

#define NEXT_ARG (*(argv++)) + 0 * (argc--)
    __attribute__((unused)) const char *program = NEXT_ARG;
    vt vt = {0};

    /* FIXME process args */

    vt_setup_io(&vt);
    vt_resize_window(&vt);

    int ret = 0;
    if (argc) {
       ret = vt_setup_child(&vt, argv);
       if (ret) return ret;
    }

    ret = vt_main_loop(&vt);
    fprintf(stderr, "ret = %d\n", ret);
    if (vt.child_pid) {
       if (vt.stdout[1] > 0) close(vt.stdout[0]);
       if (vt.stderr[1] > 0) close(vt.stderr[0]);
       int wstatus;
       if (waitpid(vt.child_pid, &wstatus, 0) != vt.child_pid) {
          perror("waitpid(child)");
       }
       vt.child_pid = 0;
    }
    vt_restore_io(&vt);
    vt_free(&vt);
    return ret;
#undef NEXT_ARG
}
