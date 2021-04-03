CC=gcc
CFLAGS=-Wall -Werror -Wextra -O2 -g

all: oss user

oss: oss.c oss.h
	$(CC) $(CFLAGS) oss.c -o oss

user: user.c oss.h
	$(CC) $(CFLAGS) user.c -o user

clean:
	rm -f oss user *.log
