CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pedantic
LIBS ?= -lcurl
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
TARGET ?= solcli

.PHONY: all install clean

all: $(TARGET)

$(TARGET): solcli.c
	$(CC) $(CFLAGS) -o $(TARGET) solcli.c $(LIBS)

install: $(TARGET)
	mkdir -p $(BINDIR)
	cp $(TARGET) $(BINDIR)/$(TARGET)
	chmod +x $(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
