#
# critbit89 - A crit-bit tree implementation for strings in C89
# Written by Jonas Gehring <jonas@jgehring.net>
#

CC = c89
CFLAGS = -Wall -pedantic -g $(ADD_CFLAGS)
LDFLAGS = $(ADD_LDFLAGS)
LIBS = 

all: test

test: critbit.o test.o
	$(CC) $(LDFLAGS) critbit.o test.o $(LIBS) -o test

critbit.o: critbit.h Makefile
test.o: critbit.h Makefile

.c.o:
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o *.gcda *.gcno test
