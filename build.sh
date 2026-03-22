#!/bin/bash

set -e 

gcc -o game ./main.c -ggdb -Wall -std=c99 -D_DEFAULT_SOURCE -Wno-missing-braces -Wunused-result -O0 -D_DEFAULT_SOURCE -I. -I/home/unix-uname/game-engines/raylib/src -I/home/unix-uname/game-engines/raylib/src/external  -I/usr/local/include -L. -L/home/unix-uname/game-engines/raylib/src -L/home/unix-uname/game-engines/raylib/src -L/usr/local/lib -lraylib -lGL -lm -lpthread -ldl -lrt -lX11 -latomic -DPLATFORM_DESKTOP -DPLATFORM_DESKTOP_GLFW

