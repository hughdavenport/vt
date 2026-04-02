// compile: gcc -Wall -O2 -o parent_cat parent_cat.c
// run: ./parent_cat

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>

int main(void) {
    int to_child[2];   // parent -> child (parent writes)
    int from_child[2]; // child -> parent (parent reads)
    pid_t pid;

    if (pipe(to_child) == -1) { perror("pipe to_child"); exit(1); }
    if (pipe(from_child) == -1) { perror("pipe from_child"); exit(1); }

    pid = fork();
    if (pid == -1) { perror("fork"); exit(1); }

    if (pid == 0) {
        // Child: wire up pipes to stdin/stdout and exec "cat"
        // Close unused ends first
        close(to_child[1]);    // child doesn't write to this pipe
        close(from_child[0]);  // child doesn't read from this pipe

        // dup read end of to_child to STDIN
        if (dup2(to_child[0], STDIN_FILENO) == -1) { perror("dup2 stdin"); _exit(1); }
        // dup write end of from_child to STDOUT
        if (dup2(from_child[1], STDOUT_FILENO) == -1) { perror("dup2 stdout"); _exit(1); }

        // close originals after dup2
        close(to_child[0]);
        close(from_child[1]);

        // exec cat
        execlp("cat", "cat", (char *)NULL);
        // if exec fails:
        perror("execlp");
        _exit(1);
    }

    // Parent
    // Close unused ends
    close(to_child[0]);    // parent doesn't read from this pipe
    close(from_child[1]);  // parent doesn't write to this pipe

    // Read one line from user's stdin, send to child, then read child's response
    char buf[4096];
    ssize_t n;

    // Prompt and read from user
    fputs("Enter a line: ", stdout);
    fflush(stdout);
    if (fgets(buf, sizeof buf, stdin) == NULL) {
        // EOF or error
        if (feof(stdin)) {
            // Close write end to signal EOF to child
            close(to_child[1]);
        } else {
            perror("fgets");
            close(to_child[1]);
        }
        // Wait for child then exit
        waitpid(pid, NULL, 0);
        return 0;
    }

    // Write the user's input to the child (cat's stdin)
    size_t len = strlen(buf);
    ssize_t written = 0;
    while (written < (ssize_t)len) {
        n = write(to_child[1], buf + written, len - written);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("write to child");
            break;
        }
        written += n;
    }

    // Close the write end to indicate EOF to cat (so cat will flush and exit if appropriate)
    close(to_child[1]);

    // Read from child's stdout and write to parent's stdout
    while ((n = read(from_child[0], buf, sizeof buf)) > 0) {
        ssize_t out = 0;
        while (out < n) {
            ssize_t w = write(STDOUT_FILENO, buf + out, n - out);
            if (w == -1) {
                if (errno == EINTR) continue;
                perror("write to stdout");
                break;
            }
            out += w;
        }
    }
    if (n == -1) perror("read from child");

    close(from_child[0]);

    // Wait for child to exit
    waitpid(pid, NULL, 0);
    return 0;
}
