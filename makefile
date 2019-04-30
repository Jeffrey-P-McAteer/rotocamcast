all: rotocamcast

rotocamcast: main.c
	gcc -O3 -std=gnu99 -o rotocamcast main.c -lX11 -lGL -lGLU

