CC     ?= cc
CFLAGS ?= -O0 -g -Wall -Wextra
LDLIBS ?= -lm
SRCS := $(wildcard *.c)
BINS := $(SRCS:.c=)

all: $(BINS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(BINS)

.PHONY: all clean
