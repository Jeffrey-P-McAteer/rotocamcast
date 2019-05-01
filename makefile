all: rotocamcast

rotocamcast: main.c
	gcc -std=gnu99 -o rotocamcast main.c -lX11 -lGL -lGLU -lpthread

