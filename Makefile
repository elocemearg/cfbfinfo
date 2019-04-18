CC=gcc
CFLAGS=-Wall -g

cfbfinfo: cfbf_main.c cfbf_file.c cfbf_walk.c cfbf_fat.c cfbf_dir.c cfbf_publisher_text.c cfbf.h
	$(CC) $(CFLAGS) -o $@ cfbf_main.c cfbf_file.c cfbf_walk.c cfbf_fat.c cfbf_dir.c cfbf_publisher_text.c
