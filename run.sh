#!/bin/bash

trap id TSTP
gcc -Wall -Wextra -Werror -Wpedantic -fsanitize=address -ggdb vt.c -o a.out
./a.out 2>tmp "$@"
cat tmp
