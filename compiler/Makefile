CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11
TARGET  = wombat-compiler
SRCS    = main.c parse.c sdb.c emit.c lex.c symtab.c enumdb.c funcdb.c
OBJS    = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
