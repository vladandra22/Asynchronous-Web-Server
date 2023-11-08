CC = gcc -g -DDEBUG -Wall

build: aws.o sock_util.o http_parser.o
	$(CC) -o aws -I. aws.o sock_util.o http_parser.o -laio

aws.o: aws.c
	$(CC) -c aws.c 

sock_util.o: sock_util.c
	$(CC) -c sock_util.c

http_parser.o: http_parser.c
	$(CC) -c http_parser.c

.PHONY: clean

clean:
	rm -f *.o aws