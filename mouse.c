#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#define UNREACHABLE(fmt, ...) do { fprintf(stderr, "%s:%d: UNREACHABLE: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } while (false)

#define MODES_LIST \
    X(MODE_NONE)          O("none")          M(0) \
    X(MODE_CLICK)         O("click")         M(9) \
    X(MODE_PRESS_RELEASE) O("press_release") M(1000) \
    X(MODE_HIGHLIGHT)     O("highlight")     M(1001) \
    X(MODE_DRAG)          O("drag")          M(1002) \
    X(MODE_MOVEMENT)      O("movement")      M(1003)

#define REPORTS_LIST \
    X(REPORT_DEFAULT)   O("default")   M(0) \
    X(REPORT_MULTIBYTE) O("multibyte") M(1005) \
    X(REPORT_DIGITS)    O("digits")    M(1006) \
    X(REPORT_URXVT)     O("urxvt")     M(1015)

#define X(name) name, 
#define O(str)
#define M(mode)
typedef enum { MODES_LIST NUM_MODES } mode_t;
typedef enum { REPORTS_LIST NUM_REPORTS } report_t;
#undef X
#undef O
#undef M

#define X(str)
#define O(str) str, 
#define M(mode)
static const char *mode_strings[] = { MODES_LIST };
static const char *report_strings[] = { REPORTS_LIST };
#undef X
#undef O
#undef M

#define X(str)
#define O(str) "|" str
#define M(mode)
static const char *all_modes = (MODES_LIST) + 1;
static const char *all_reports = (REPORTS_LIST) + 1;
#undef X
#undef O
#undef M

#define X(str)
#define O(str)
#define M(mode) mode, 
static const uint16_t mode_codes[] = { MODES_LIST };
static const uint16_t report_codes[] = { REPORTS_LIST };
#undef X
#undef O
#undef M

#define COLORS_LIST \
    X(BLACK) \
    X(RED) \
    X(GREEN) \
    X(YELLOW) \
    X(BLUE) \
    X(MAGENTA) \
    X(CYAN) \
    X(WHITE)

#define X(col) col, 
typedef enum { COLORS_LIST NUM_COLORS } color_t;
#undef X

#define BOLD 1
#define FOREGROUND 30
#define BACKGROUND 40

int hexdump_color(uint8_t b)
{
    switch (b) {
        case 0:
            return WHITE;

        case '\t':
        case '\n':
        case '\r':
            return YELLOW;

        case 0xff:
            return BLUE;

        default:
            return isprint(b) ? GREEN : RED;
    }
}

int hexdump(FILE *stream, uint8_t *bytes, size_t count, bool color)
{
    int ret = 0;
#define print(...) do { int printed = fprintf(stream, __VA_ARGS__); if (printed == -1) return -1; ret += printed; } while (false)

    for (size_t y = 0; y <= count / 16; y ++) {
        if (y == (count / 16) && (count % 16) == 0) break;
        print("%07lx0:", y);
        for (size_t x = 0; x < (y == (count / 16) ? (count % 16) : 16); x ++) {
            uint8_t b = bytes[y * 16 + x];
            if (x % 2 == 0) print(" ");
            if (color) print("\033[%u;%um", BOLD, FOREGROUND + hexdump_color(b));
            print("%02x", b);
            if (color) print("\033[0m");
        }

        if (y == (count / 16)) {
            for (size_t x = count % 16; x < 16; x ++) {
                if (x % 2 == 0) print(" ");
                print("  ");
            }
        }

        print("  ");
        for (size_t x = 0; x < (y == (count / 16) ? (count % 16) : 16); x ++) {
            uint8_t b = bytes[y * 16 + x];
            if (color) print("\033[%u;%um", BOLD, FOREGROUND + hexdump_color(b));
            print("%c", isprint(b) ? b : '.');
            if (color) print("\033[0m");
        }
        print("\n");
    }
#undef print
    fflush(stream);

    return ret;
}

#define BUTTONS_LIST \
    X(LEFT_BUTTON)        S("left button") \
    X(MIDDLE_BUTTON)      S("middle button") \
    X(RIGHT_BUTTON)       S("right button") \
    X(NO_BUTTON)          S("no button") \
    X(WHEEL_UP)           S("wheel up") \
    X(WHEEL_DOWN)         S("wheel down") \
    X(WHEEL_LEFT)         S("wheel left") \
    X(WHEEL_RIGHT)        S("wheel right") \
    X(ADDITIONAL_BUTTONS) S("TODO additional buttons")

#define X(code) code, 
#define S(str)
typedef enum { BUTTONS_LIST NUM_BUTTONS } mouse_button;
#undef X
#undef S

#define X(code)
#define S(str) str, 
static const char *mouse_button_strings[] = { BUTTONS_LIST "" };
#undef X
#undef S

typedef enum
{
    NO_MODIFIER = 0,
    SHIFT       = 1 << 0,
    ALT         = 1 << 1,
    CONTROL     = 1 << 2,
} key_modifier;

typedef struct
{
    size_t row;
    size_t col;
    mouse_button button;
    key_modifier modifiers;
    bool moving;
    bool release;
    bool was_down;
} mouse_event;

int print_key(FILE *stream, mouse_event event)
{
    int ret = 0;
#define print(...) do { int printed = fprintf(stream, __VA_ARGS__); if (printed == -1) return -1; ret += printed; } while (false)

    if (event.button <= NO_BUTTON) {
        print("Mouse %s", event.release ? "release" : (event.moving ? (event.was_down ? "dragging movement" : "movement") : "press"));

        if (event.button != NO_BUTTON) {
            print(" with %s", mouse_button_strings[event.button]);
        }
    } else {
        print("Mouse %s", mouse_button_strings[event.button]);
    }

    if (event.modifiers != NO_MODIFIER) {
        print(" %s", event.button == NO_BUTTON ? "and" : "with");
        key_modifier modifiers = event.modifiers;
        while (modifiers) {
            print(modifiers == event.modifiers ? " " : "+");
            if (modifiers & CONTROL) {
                print("control");
                modifiers &= ~CONTROL;
                continue;
            }
            if (modifiers & ALT) {
                print("alt");
                modifiers &= ~ALT;
                continue;
            }
            if (modifiers & SHIFT) {
                print("shift");
                modifiers &= ~SHIFT;
                continue;
            }
            UNREACHABLE("Invalid modifier 0x%02x", modifiers);
        }
    }

    print(" at %lu%sx%lu%s\n", event.col, event.col == 224 ? "+" : "", event.row, event.row == 224 ? "+" : "");

#undef print
    return ret;
}

void process_btn(int btn, mouse_event *event)
{
    if (btn & 4) {
        event->modifiers |= SHIFT;
        btn &= ~4;
    }
    if (btn & 8) {
        event->modifiers |= ALT;
        btn &= ~8;
    }
    if (btn & 16) {
        event->modifiers |= CONTROL;
        btn &= ~16;
    }
    if (btn & 32) {
        event->moving = true;
        btn &= ~32;
    }
    if (btn & 64) {
        event->button = 4;
        btn &= ~64;
    }
    if (btn & 128) {
        event->button = ADDITIONAL_BUTTONS;
        btn &= ~128;
    }
    event->button |= btn & 0x3;
    if (event->button == NO_BUTTON && !event->moving) {
        event->release = true;
    }
}

int main(int argc, char **argv)
{
#define HAS_ARG argc
#define ARG *(argv++ + (argc-- ? 0 : 0))

    const char *prog = ARG;
    mode_t mode = MODE_NONE;
    report_t report = REPORT_DEFAULT;

    while (HAS_ARG) {
        const char *arg = ARG;
        do {
            size_t i;

            if (!mode) {
                for (i = 0; i < NUM_MODES; i ++) {
                    if (strcmp(mode_strings[i], arg) == 0) {
                        mode = (mode_t)i;
                        break;
                    }
                }
                if (i != NUM_MODES) break;
            }

            if (!report) {
                for (i = 0; i < NUM_REPORTS; i ++) {
                    if (strcmp(report_strings[i], arg) == 0) {
                        report = (report_t)i;
                        break;
                    }
                }
                if (i != NUM_REPORTS) break;
            }

            if (strcmp("--help", arg) && strcmp("help", arg)) {
                fprintf(stderr, "Unknown option: %s\n", arg);
            }
            fprintf(stderr, "Usage: %s [%s] [%s]\n", prog, all_modes, all_reports);
            fflush(stderr);
            return 1;
        } while (false);
    }
#undef ARG

    int ret = 0;
#define return_defer(code) do { ret = (code); goto defer; } while (false)

    struct termios orig_ios;
    if (tcgetattr(STDIN_FILENO, &orig_ios) == 0) {
        struct termios new_ios = orig_ios;
        cfmakeraw(&new_ios);
        new_ios.c_oflag |= OPOST;
        new_ios.c_oflag |= ONLCR;
        tcsetattr(STDIN_FILENO, TCSANOW, &new_ios);
    }

    setbuf(stdin, NULL);
    setbuf(stdout, NULL);

    printf("Listening with mode %s for reports in %s format\n", mode_strings[mode], report_strings[report]);

    if (mode_codes[mode]) printf("\033[?%uh", mode_codes[mode]);
    if (report_codes[report]) printf("\033[?%uh", report_codes[report]);

    bool was_down = true;

    while (true) {
        uint8_t bytes[512];
        ssize_t red = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (red == -1) {
            perror("read()");
            break;
        }
        if (red == 0) {
            break;
        }

        fprintf(stderr, "Received %lu bytes\n", red);
        hexdump(stderr, bytes, (unsigned)red, true);

        uint8_t *buf = bytes;
        if (red == 1 && *buf == '\003') break;
#define NEXT *(buf++ + (red-- ? 0 : 0))
        while (red) {
            uint8_t b = NEXT;
            if (b == '\003') break; // Ctrl-C (not SIGINT from cfmakeraw()
            if (b != '\033') continue; // Not escape sequence, so not mouse sequence, ignore
            if (!red) break; // Not escape sequence, so not mouse sequence, ignore

            b = NEXT;
            if (b != '[') continue; // Not escape sequence, so not mouse sequence, ignore
            if (!red) break; // Not escape sequence, so not mouse sequence, ignore

            mouse_event event = {.was_down = was_down};
            int col, row, btn;

            switch (report) {
                case REPORT_DEFAULT:
                    // We are expecting \e[M%c%c%c where the three bytes are btn, column, row
                    // btn is then parsed into an actual button (or none), and flags like movement and release
                    // release events do not come with button information
                    b = NEXT;
                    if (b != 'M') continue; // Not escape sequence, so not mouse sequence, ignore
                    if (!red) break; // Not escape sequence, so not mouse sequence, ignore
                    if (red < 3) break; // Not enough for valid sequence, ingnore

                    btn = NEXT;
                    if (btn == 0) break; // Invalid null button
                    if (btn < 32) break; // Invalid button value not offset by 32 for default report
                    btn -= 32;

                    col = NEXT;
                    if (col == 0) {
                        col = 224;
                    } else if (col > 32) {
                        col -= 32;
                    } else {
                        break; // Invalid column value not offset by 32 for default report
                    }

                    row = NEXT;
                    if (row == 0) {
                        row = 224;
                    } else if (row > 32) {
                        row -= 32;
                    } else {
                        break; // Invalid row value not offset by 32 for default report
                    }
                    break;

                case REPORT_MULTIBYTE:
                    fprintf(stderr, "UNIMPLEMENTED MULTI\n"); return_defer(1);

#define READ_CSI_PARAM(var) do { \
    var = NEXT; \
    if (!isdigit(var)) { \
        var = -1; \
        break; \
    } \
    var -= '0'; \
    while (red) { \
        b = NEXT; \
        if (!isdigit(b)) break; \
        if (!red) break; \
        var *= 10; \
        var += b - '0'; \
    } \
} while (false)

                case REPORT_DIGITS:
                    // We are expecting \e[<%u;%u;%u%c where the three params are btn, column, row, and the terminating character is M for button press and movement, and m for button releases
                    // btn is then parsed into an actual button (or none), and flags like movement
                    // release events come with button information with this mode
                    b = NEXT;
                    if (b != '<') continue; // Not escape sequence, so not mouse sequence, ignore
                    if (!red) break; // Not escape sequence, so not mouse sequence, ignore
                    if (red < 6) break; // Not enough for valid sequence, ingnore

                    READ_CSI_PARAM(btn);
                    if (btn == -1) break;
                    if (b != ';') break;
                    if (!red) break;

                    READ_CSI_PARAM(col);
                    if (col == -1) break;
                    if (b != ';') break;
                    if (!red) break;

                    READ_CSI_PARAM(row);
                    if (row == -1) break;
                    if (b == 'm') {
                        event.release = true;
                    } else if (b != 'M') {
                        col = 0;
                        row = 0;
                        break; // Not valid terminator for mouse sequence, different CSI sequence, ignore
                    }
                    break;

                case REPORT_URXVT:
                    // We are expecting \e[%u;%u;%uM where the three params are btn, column, row
                    // btn is offset by 32, then parsed into an actual button (or none), and flags like movement
                    // release events lose button information
                    if (red < 6) break; // Not enough for valid sequence, ingnore

                    READ_CSI_PARAM(btn);
                    if (btn == -1) break;
                    if (b != ';') break;
                    if (!red) break;
                    btn -= 32;

                    READ_CSI_PARAM(col);
                    if (col == -1) break;
                    if (b != ';') break;
                    if (!red) break;

                    READ_CSI_PARAM(row);
                    if (row == -1) break;
                    if (b != 'M') {
                        col = 0;
                        row = 0;
                        break; // Not valid terminator for mouse sequence, different CSI sequence, ignore
                    }
                    break;

#undef READ_CSI_PARAM
                default:
                    UNREACHABLE("Invalid report type %d", report);
                    return_defer(1);
            }
            if (!col || !row) continue;

            process_btn(btn, &event);
            event.col = col;
            event.row = row;
            event.was_down = was_down = !(event.release || event.button >= NO_BUTTON);
            print_key(stdout, event);
        }
#undef NEXT
    }

#undef return_defer
defer:

    tcsetattr(STDIN_FILENO, TCSANOW, &orig_ios);

    if (mode_codes[mode]) printf("\033[?%ul", mode_codes[mode]);
    if (report_codes[report]) printf("\033[?%ul", report_codes[report]);

    return ret;
}
