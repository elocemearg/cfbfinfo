CC=gcc
CFLAGS=-Wall -g

cfbf: cfbf.c cfbf_file.c cfbf_walk.c cfbf_fat.c cfbf_dir.c cfbf_publisher_text.c cfbf.h
	$(CC) $(CFLAGS) -o $@ cfbf.c cfbf_file.c cfbf_walk.c cfbf_fat.c cfbf_dir.c cfbf_publisher_text.c
