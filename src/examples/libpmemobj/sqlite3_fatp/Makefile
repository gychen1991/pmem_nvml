DBG=
CC = gcc
CFLAGS	= -w -g
INCDIR ?= ../../../include
LIBDIR ?= ../../../debug

INCS = -I$(INCDIR)
LIBS = -Wl,-rpath=$(LIBDIR) -L$(LIBDIR) -lpmemobj -lpthread -ldl

all:
	gcc $(INCS) $(CFLAGS) -o sqlite3 shell.c sqlite3.c $(LIBS)

