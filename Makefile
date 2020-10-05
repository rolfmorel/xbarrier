CC = cc
CFLAGS = -Wall -lm

ifneq ($(DEBUG),)
CFLAGS += -ggdb3 -DDEBUG=1
else
CFLAGS += -O2
endif

XSUBLIBS = x11 xi xfixes 
XCFLAGS := $(shell pkg-config --cflags --libs $(XSUBLIBS))

CFLAGS += $(XCFLAGS)

default: xfence

xfence: xfence.c
	$(CC) -o $@ $< $(CFLAGS)

.PHONY: clean
clean:
	rm -f xfence 
