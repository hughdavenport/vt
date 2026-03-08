#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <unistd.h>

#define UNIMPL(fmt, ...) do { fprintf(stderr, "%s:%d: \033[31mUNIMPLEMENTED\033[m: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); fflush(stderr); abort(); } while (false)
#define UNREACHABLE(fmt, ...) do { fprintf(stderr, "%s:%d: UNREACHABLE: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); fflush(stderr); abort(); } while (false)

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
    X(VT_ACTION_IGNORE) \
    X(VT_ACTION_PRINT) \
    X(VT_ACTION_EXECUTE) \
    X(VT_ACTION_CLEAR) \
    X(VT_ACTION_COLLECT) \
    X(VT_ACTION_PARAM) \
    X(VT_ACTION_ESC_DISPATCH) \
    X(VT_ACTION_CSI_DISPATCH) \
    X(VT_ACTION_HOOK) \
    X(VT_ACTION_PUT) \
    X(VT_ACTION_UNHOOK) \
    X(VT_ACTION_OSC_START) \
    X(VT_ACTION_OSC_PUT) \
    X(VT_ACTION_OSC_END)

#define VT_EXECUTE_FUNCTIONS_LIST \
   C(0x00) X(VT_EXECUTE_NULL)   S(UNIMPL("VT_EXECUTE_NULL")) \
   C(0x05) X(VT_EXECUTE_ENQ)    S(UNIMPL("VT_EXECUTE_ENQ")) \
   C(0x07) X(VT_EXECUTE_BEL)    S(UNIMPL("VT_EXECUTE_BEL")) \
   C(0x08) X(VT_EXECUTE_BS)     S(UNIMPL("VT_EXECUTE_BS")) \
   C(0x09) X(VT_EXECUTE_HT)     S(UNIMPL("VT_EXECUTE_HT")) \
   C(0x0A) X(VT_EXECUTE_LF)     S(UNIMPL("VT_EXECUTE_LF")) \
   C(0x0B) X(VT_EXECUTE_VT)     S(UNIMPL("VT_EXECUTE_VT")) \
   C(0x0C) X(VT_EXECUTE_FF)     S(UNIMPL("VT_EXECUTE_FF")) \
   C(0x0D) X(VT_EXECUTE_CR)     S(UNIMPL("VT_EXECUTE_CR")) \
   C(0x0E) X(VT_EXECUTE_SO)     S(UNIMPL("VT_EXECUTE_SO")) \
   C(0x0F) X(VT_EXECUTE_SI)     S(UNIMPL("VT_EXECUTE_SI")) \
   C(0x11) X(VT_EXECUTE_DC1)    S(UNIMPL("VT_EXECUTE_DC1")) \
   C(0x13) X(VT_EXECUTE_DC3)    S(UNIMPL("VT_EXECUTE_DC3")) \
   C(0x18) X(VT_EXECUTE_CAN)    S(UNIMPL("VT_EXECUTE_CAN")) \
   C(0x1A) X(VT_EXECUTE_SUB)    S(UNIMPL("VT_EXECUTE_SUB")) \
   C(0x1B) X(VT_EXECUTE_ESC)    S(UNIMPL("VT_EXECUTE_ESC")) /* UNREACHABLE, mapped in "anywhere" transitions */ \
   C(0x7F) X(VT_EXECUTE_DEL)    S(UNIMPL("VT_EXECUTE_DEL")) /* UNREACHABLE, used only in VT_ACTION_PRINT or VT_ACTION_IGNORE not VT_ACTION_EXECUTE */ \
   C(0x84) X(VT_EXECUTE_IND)    S(UNIMPL("VT_EXECUTE_IND")) \
   C(0x85) X(VT_EXECUTE_NEL)    S(UNIMPL("VT_EXECUTE_NEL")) \
   C(0x88) X(VT_EXECUTE_HTS)    S(UNIMPL("VT_EXECUTE_HTS")) \
   C(0x8D) X(VT_EXECUTE_RI)     S(UNIMPL("VT_EXECUTE_RI")) \
   C(0x8E) X(VT_EXECUTE_SS2)    S(UNIMPL("VT_EXECUTE_SS2")) \
   C(0x8F) X(VT_EXECUTE_SS3)    S(UNIMPL("VT_EXECUTE_SS3")) \
   C(0x90) X(VT_EXECUTE_DCS)    S(UNIMPL("VT_EXECUTE_DCS")) \
   C(0x98) X(VT_EXECUTE_SOS)    S(UNIMPL("VT_EXECUTE_SOS")) \
   C(0x9A) X(VT_EXECUTE_DECID)  S(UNIMPL("VT_EXECUTE_DECID")) \
   C(0x9B) X(VT_EXECUTE_CSI)    S(UNIMPL("VT_EXECUTE_CSI")) /* UNREACHABLE, mapped in "anywhere" transitions */ \
   C(0x9C) X(VT_EXECUTE_ST)     S(UNIMPL("VT_EXECUTE_ST")) \
   C(0x9D) X(VT_EXECUTE_OSC)    S(UNIMPL("VT_EXECUTE_OSC")) /* UNREACHABLE, mapped in "anywhere" transitions */ \
   C(0x9E) X(VT_EXECUTE_PM)     S(UNIMPL("VT_EXECUTE_PM")) \
   C(0x9F) X(VT_EXECUTE_APC)    S(UNIMPL("VT_EXECUTE_APC"))

#define C(code)
#define S(code)

#define X(name) name, 
typedef enum { VT_STATES_LIST VT_NUM_STATES } vt_state;
typedef enum { VT_ACTIONS_LIST VT_NUM_ACTIONS } vt_action;
typedef enum { VT_EXECUTE_FUNCTIONS_LIST VT_NUM_EXECUTE_FUNCTIONS } vt_execute_function;
#undef X

#define X(name) [name] = #name, 
static const char *vt_state_strings[] = { VT_STATES_LIST };
static const char *vt_action_strings[] = { VT_ACTIONS_LIST };
static const char *vt_execute_function_strings[] = { VT_EXECUTE_FUNCTIONS_LIST };

#define VT_STATE_STRING(stat) (((stat) >= 0 && (stat) < VT_NUM_STATES) ? vt_state_strings[(stat)] : "(state out of bounds)")
#define VT_ACTION_STRING(act) (((act) >= 0 && (act) < VT_NUM_ACTIONS) ? vt_action_strings[(act)] : "(action out of bounds)")
#define VT_EXECUTE_FUNCTION_STRING(func) (((func) >= 0 && (func) < VT_NUM_EXECUTE_FUNCTIONS) ? vt_execute_function_strings[(func)] : "(execute_function out of bounds)")
#undef X

#undef C

static vt_execute_function _vt_execute_function(uint8_t input)
{
    switch (input) {
#define C(code) case code:
#define X(name) return name;
        VT_EXECUTE_FUNCTIONS_LIST
#undef X
#undef C
    }
    UNREACHABLE("Unexpected function input 0x%02X", input);
}

#define C(code)

static_assert(VT_NUM_STATES == 14, "Not the same number as William's design");
static_assert(VT_NUM_ACTIONS == 14, "Not the same number as William's design");

typedef struct
{
    vt_state state;
    struct termios original_ios;
    bool raw;
    bool nonblocking;
    FILE *tty;
    struct winsize winsize;
} vt;

void vt_process(vt *vt, uint8_t input);

#undef S

void _vt_execute(vt *vt, vt_execute_function func)
{
    if (!vt) return;

    fprintf(stderr, "state %s, execute %s\n", VT_STATE_STRING(vt->state), VT_EXECUTE_FUNCTION_STRING(func));

    /* all cases must return */
    static_assert(VT_NUM_EXECUTE_FUNCTIONS == 31, "Not all functions handled");
    switch (func) {
#define X(name) case name:
#define S(code) code; return;
        VT_EXECUTE_FUNCTIONS_LIST
#undef X
#undef S
        case VT_NUM_EXECUTE_FUNCTIONS: break;
    }
    UNREACHABLE("Unexpected func %d", func);
}

void _vt_action(vt *vt, vt_action action, __attribute__((unused)) uint8_t input)
{
    if (!vt) return;

    fprintf(stderr, "state %s, action %s, input %02X\n", VT_STATE_STRING(vt->state), VT_ACTION_STRING(action), input);

    /* all cases must return */
    static_assert(VT_NUM_ACTIONS == 14, "Not all actions handled");
    switch (action) {
        case VT_ACTION_IGNORE: UNIMPL("VT_ACTION_IGNORE");
        case VT_ACTION_PRINT: UNIMPL("VT_ACTION_PRINT");
        case VT_ACTION_EXECUTE: _vt_execute(vt, _vt_execute_function(input)); return;
        case VT_ACTION_CLEAR: UNIMPL("VT_ACTION_CLEAR");
        case VT_ACTION_COLLECT: UNIMPL("VT_ACTION_COLLECT");
        case VT_ACTION_PARAM: UNIMPL("VT_ACTION_PARAM");
        case VT_ACTION_ESC_DISPATCH: UNIMPL("VT_ACTION_ESC_DISPATCH");
        case VT_ACTION_CSI_DISPATCH: UNIMPL("VT_ACTION_CSI_DISPATCH");
        case VT_ACTION_HOOK: UNIMPL("VT_ACTION_HOOK");
        case VT_ACTION_PUT: UNIMPL("VT_ACTION_PUT");
        case VT_ACTION_UNHOOK: UNIMPL("VT_ACTION_UNHOOK");
        case VT_ACTION_OSC_START: UNIMPL("VT_ACTION_OSC_START");
        case VT_ACTION_OSC_PUT: UNIMPL("VT_ACTION_OSC_PUT");
        case VT_ACTION_OSC_END: UNIMPL("VT_ACTION_OSC_END");

        case VT_NUM_ACTIONS: break;
    }
    UNREACHABLE("Unexpected action %d", action);
}

void _vt_transition(vt *vt, vt_state to)
{
    if (!vt) return;

    fprintf(stderr, "transition from state %s to state %s\n", VT_STATE_STRING(vt->state), VT_STATE_STRING(to));

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
            _vt_transition(vt, VT_STATE_GROUND);
            return;

        case 0x1B: _vt_transition(vt, VT_STATE_ESCAPE); return;
        case 0x90: _vt_transition(vt, VT_STATE_DCS_ENTRY); return;

        case 0x98: case 0x9E: case 0x9F:
            _vt_transition(vt, VT_STATE_SOS_PM_APC_STRING);
            return;

        case 0x9B: _vt_transition(vt, VT_STATE_CSI_ENTRY); return;
        case 0x9D: _vt_transition(vt, VT_STATE_OSC_STRING); return;
    }

    /* all cases must return */
    static_assert(VT_NUM_STATES == 14, "Not all states handled");
    switch (vt->state) {
        case VT_STATE_GROUND:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:

                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
                    _vt_action(vt, VT_ACTION_PRINT, input);
                    break;
            }
            return;

        case VT_STATE_ESCAPE:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_ESCAPE_INTERMEDIATE);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x59: case 0x5A:
                    _vt_action(vt, VT_ACTION_ESC_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND);
                    break;

                case 0x50: _vt_transition(vt, VT_STATE_DCS_ENTRY); break;
                case 0x5D: _vt_transition(vt, VT_STATE_OSC_STRING); break;
                case 0x5B: _vt_transition(vt, VT_STATE_CSI_ENTRY); break;

                case 0x58: case 0x5E: case 0x5F:
                    _vt_transition(vt, VT_STATE_SOS_PM_APC_STRING);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_ESCAPE_INTERMEDIATE:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:

                    _vt_action(vt, VT_ACTION_ESC_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_OSC_STRING:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:

                    _vt_action(vt, VT_ACTION_OSC_PUT, input);
                    break;

                case 0x9C: _vt_transition(vt, VT_STATE_GROUND); break;
            }
            return;

        case VT_STATE_CSI_ENTRY:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_CSI_INTERMEDIATE);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3B:
                    _vt_action(vt, VT_ACTION_PARAM, input);
                    _vt_transition(vt, VT_STATE_CSI_PARAM);
                    break;

                case 0x3A: _vt_transition(vt, VT_STATE_CSI_IGNORE); break;

                case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_CSI_PARAM);
                    break;

                case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:

                    _vt_action(vt, VT_ACTION_CSI_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_CSI_PARAM:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_CSI_INTERMEDIATE);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3B:
                    _vt_action(vt, VT_ACTION_PARAM, input);
                    break;

                case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_transition(vt, VT_STATE_CSI_IGNORE);
                    break;

                case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:

                    _vt_action(vt, VT_ACTION_CSI_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_CSI_IGNORE:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
                    _vt_transition(vt, VT_STATE_GROUND);
                    break;
            }
            return;

        case VT_STATE_CSI_INTERMEDIATE:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                    _vt_action(vt, VT_ACTION_EXECUTE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_transition(vt, VT_STATE_CSI_IGNORE);
                    break;

                case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:

                    _vt_action(vt, VT_ACTION_CSI_DISPATCH, input);
                    _vt_transition(vt, VT_STATE_GROUND);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;
            }
            return;

        case VT_STATE_SOS_PM_APC_STRING:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                case 0x9C: _vt_transition(vt, VT_STATE_GROUND); break;
            }
            return;

        case VT_STATE_DCS_ENTRY:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_DCS_INTERMEDIATE);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3B:
                    _vt_action(vt, VT_ACTION_PARAM, input);
                    _vt_transition(vt, VT_STATE_DCS_PARAM);
                    break;

                case 0x3A: _vt_transition(vt, VT_STATE_DCS_IGNORE); break;

                case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_DCS_PARAM);
                    break;

                case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:

                    _vt_transition(vt, VT_STATE_DCS_PASSTHROUGH);
                    break;
            }
            return;

        case VT_STATE_DCS_IGNORE:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                case 0x9C: _vt_transition(vt, VT_STATE_GROUND); break;
            }
            return;

        case VT_STATE_DCS_PARAM:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    _vt_transition(vt, VT_STATE_DCS_INTERMEDIATE);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3B:
                    _vt_action(vt, VT_ACTION_PARAM, input);
                    break;

                case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_transition(vt, VT_STATE_DCS_IGNORE);
                    break;

                case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:

                    _vt_transition(vt, VT_STATE_DCS_PASSTHROUGH);
                    break;
            }
            return;

        case VT_STATE_DCS_INTERMEDIATE:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x7F:
                    _vt_action(vt, VT_ACTION_IGNORE, input);
                    break;

                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                    _vt_action(vt, VT_ACTION_COLLECT, input);
                    break;

                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                    _vt_transition(vt, VT_STATE_DCS_IGNORE);
                    break;

                case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:

                    _vt_transition(vt, VT_STATE_DCS_PASSTHROUGH);
                    break;
            }
            return;

        case VT_STATE_DCS_PASSTHROUGH:
            switch (input) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E:
                    _vt_action(vt, VT_ACTION_PUT, input);
                    break;

                case 0x7F: _vt_action(vt, VT_ACTION_IGNORE, input); break;

                case 0x9C: _vt_transition(vt, VT_STATE_GROUND); break;
            }
            return;

        case VT_NUM_STATES: break;
    }
    UNREACHABLE("Unexpected state %d", vt->state);
}

int setup_io(vt *vt)
{
    if (!vt) return -1;

    if (isatty(STDOUT_FILENO)) {
        vt->tty = stdout;
    } else if (isatty(STDERR_FILENO)) {
        vt->tty = stderr;
    } else {
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

void restore_io(vt *vt)
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

void setup_window(vt *vt)
{
    struct winsize w = {0};
    int fds[] = { STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO };
    for (size_t i = 0; i < sizeof(fds)/sizeof(*fds); i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &w) == 0 && w.ws_col && w.ws_row)
            break;
        w = (struct winsize){0};
    }

    if (w.ws_col && w.ws_row) {
        vt->winsize = w;
    } else {
        vt->winsize = (struct winsize){.ws_col = 80, .ws_row = 24};
    }
    fprintf(stderr, "window size %ux%u\n", vt->winsize.ws_col, vt->winsize.ws_row);
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
            setup_window(vt);
            return 0;
    }
    UNREACHABLE("Unhandled signal %d", signo);
}

int main_loop(vt *vt)
{
    if (!vt) return 1;

    int sigfd = _signalfd(SIGWINCH, SIGINT);
    while (true) {
        int fd = _select(sigfd, STDIN_FILENO);
        if (fd == sigfd) {
            struct signalfd_siginfo siginfo;
            ssize_t red = read(sigfd, &siginfo, sizeof(siginfo));
            if (red != sizeof(siginfo)) {
                perror("read(sigfd)");
                return 1;
            }

            int ret = handle_signal(vt, siginfo.ssi_signo);
            if (ret) return ret;
        } else {
            uint8_t buf[512];
            ssize_t red = read(STDIN_FILENO, buf, sizeof(buf));
            fprintf(stderr, "red = %ld\n", red);
            if (red == -1) {
                perror("read()");
                return 1;
            } else if (red == 0) {
                break;
            }
            for (size_t i = 0; i < (unsigned)red; i ++) {
                printf("vt_process(.., 0x%02X)\n", buf[i]);
                vt_process(vt, buf[i]);
                printf("state now %s\n", VT_STATE_STRING(vt->state));
            }
        }
    }

    fprintf(stderr, "EOF\n");
    return 0;
}
int main(__attribute__((unused)) int argc, __attribute__((unused)) char **argv)
{
    vt vt = {0};
    setup_io(&vt);
    setup_window(&vt);
    int ret = main_loop(&vt);
    restore_io(&vt);
    return ret;
}
