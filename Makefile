CC       ?= gcc
CSTD     ?= gnu23

TARGET   := pictura-stainless
SRC      := pictura-stainless.c
OBJ      := $(SRC:.c=.o)

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

CFLAGS   ?= -std=$(CSTD) -O2 -flto -march=native -pipe \
            -fomit-frame-pointer -fno-stack-protector -no-pie \
            -Wall -Wextra -Wpedantic \
            -D_GNU_SOURCE

LDFLAGS  ?= -flto -no-pie -Wl,-O1,--sort-common,--as-needed

INSTALL  ?= install
RM       ?= rm -f

.PHONY: all release clean install uninstall

all: release

release: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	$(RM) $(TARGET) $(OBJ)
