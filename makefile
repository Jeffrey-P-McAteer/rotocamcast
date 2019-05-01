all: rotocamcast

rotocamcast: main.c
	gcc -g -std=gnu99 -o rotocamcast main.c -lX11 -lGL -lGLU -lpthread -lXext

