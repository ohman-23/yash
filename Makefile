CC=gcc

all: yash.o compile

yash.o: yash.c
	$(CC) -c yash.c

compile:
	$(CC) -g -o yash yash.o -lreadline

clean:
	rm yash yash.o
