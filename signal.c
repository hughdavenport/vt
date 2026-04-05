#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

int main()
{
    int ret = 0;
    fprintf(stderr, "pid %d\n", getpid());

    struct termios orig_termios;
    bool tty = false;
    if (isatty(STDIN_FILENO)) {
        tty = true;
        if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
            perror("tcgetattr()");
            return 1;
        }

        struct termios raw_termios = orig_termios;
        raw_termios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        /* raw_termios.c_oflag &= ~OPOST; */
        raw_termios.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        raw_termios.c_cflag &= ~(CSIZE | PARENB);
        raw_termios.c_cflag |= CS8;

        // Keep signals from keyboard
        raw_termios.c_lflag |= ISIG;
        raw_termios.c_cc[VINTR] = '\003';
        raw_termios.c_cc[VQUIT] = '\034';
        raw_termios.c_cc[VSUSP] = '\032';

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios) == -1) {
            perror("tcsetattr()");
            return 1;
        }
    }

    sigset_t mask;
    sigfillset(&mask);
    for (int i = SIGRTMIN; i <= SIGRTMAX; i ++) {
        sigaddset(&mask, i);
    }
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        perror("sigprocmask()");
        ret = 1;
    }

    int sigfd = -1;
    if (!ret) {
        sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
        if (sigfd == -1) {
            perror("signalfd()");
            ret = 1;
        }
    }

    int last = -1;
    while (!ret) {
        int nfds = sigfd + 1;
        fd_set readables;
        FD_ZERO(&readables);
        FD_SET(sigfd, &readables);
        FD_SET(STDIN_FILENO, &readables);

        int sel = select(nfds, &readables, NULL, NULL, NULL);
        if (sel == -1) {
            perror("select()");
            ret = 1;
            break;
        }

        if (sel && FD_ISSET(STDIN_FILENO, &readables)) {
            char buf[512] = {0};
            ssize_t red = read(STDIN_FILENO, buf, sizeof(buf));
            if (red == -1) {
                perror("read(stdin)");
                ret = 1;
                break;
            }
            fprintf(stderr, "Received stdin, exiting\n");
            if (red == 0) break;
            close(STDIN_FILENO);
            break;
        } else if (sel && FD_ISSET(sigfd, &readables)) {
            struct signalfd_siginfo siginfo;
            ssize_t red = read(sigfd, &siginfo, sizeof(siginfo));
            if (red == -1) {
                perror("read()");
                ret = 1;
                break;
            }

            if (red != sizeof(siginfo)) {
                fprintf(stderr, "Unexpected result from read: %ld (expected %ld)\n", red, sizeof(siginfo));
                ret = 1;
                break;
            }

            uint32_t sig = siginfo.ssi_signo;
            if (sig == last) {
                bool match = false;
                switch (sig) {
                    case SIGINT:
                    case SIGTERM:
                        fprintf(stderr, "Quitting as received two signals in a row %d: %s (%s)\n", sig, sigabbrev_np(sig), sigdescr_np(sig));
                        match = true;
                }
                if (match) {
                    ret = 128 + sig;
                    break;
                }
            }
            last = sig;

            if ((signed)sig >= SIGRTMIN && (signed)sig <= SIGRTMAX) {
                if ((signed)sig == SIGRTMIN) {
                    fprintf(stderr, "Received real time signal %d: SIGRTMIN\n", sig);
                } else if ((signed)sig == SIGRTMAX) {
                    fprintf(stderr, "Received real time signal %d: SIGRTMAX\n", sig);
                } else {
                    uint32_t mid = SIGRTMIN + (SIGRTMAX - SIGRTMIN) / 2;
                    if (sig <= mid) {
                        fprintf(stderr, "Received real time signal %d: SIGRTMIN+%d\n", sig, sig - SIGRTMIN);
                    } else {
                        fprintf(stderr, "Received real time signal %d: SIGRTMAX-%d\n", sig, SIGRTMAX - sig);
                    }
                }
            } else {
                fprintf(stderr, "Received signal %d: %s (%s)\n", sig, sigabbrev_np(sig), sigdescr_np(sig));
            }

        } else {
            fprintf(stderr, "Unexpected result from select: %d\n", sel);
            bool any = false;
            for (int i = 0; i < FD_SETSIZE; i ++) {
                if (FD_ISSET(i, &readables)) {
                    any = true;
                    fprintf(stderr, " %d", i);
                }
            }
            if (any) fprintf(stderr, "\n");
            ret = 1;
            break;
        }
    }

    if (tty) tcsetattr(STDOUT_FILENO, TCSANOW, &orig_termios);
    if (sigfd != -1) close(sigfd);
    return ret;
}
