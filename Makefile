# compiler
CC ?= gcc

# program name
PROG = omx-clap-host

# mod-host checkout that provides libmod-host-plumbing.a and its headers
MOD_HOST_DIR ?= ../wt-mod-host-clap
PLUMBING_LIB = $(MOD_HOST_DIR)/libmod-host-plumbing.a

# CLAP headers: pkg-config when clap-devel is installed, CLAP_CFLAGS=-I<dir> otherwise
CLAP_CFLAGS ?= $(shell pkg-config --cflags clap 2>/dev/null)

# plugin the test loads
CLAP_TEST_PLUGIN ?= ../openmixer/packages/omx-plugins/bin/omx-delay.clap

# default compiler and linker flags
CFLAGS += -O3 -Wall -Wextra -std=gnu99 -fPIC -D_GNU_SOURCE -pthread
CFLAGS += -Werror=implicit-function-declaration -Werror=return-type

ifeq ($(DEBUG), 1)
   CFLAGS += -O0 -g -DDEBUG
else
   CFLAGS += -fvisibility=hidden
   LDFLAGS += -s
endif

# libraries
LIBS = $(shell pkg-config --libs jack) -ldl -lpthread -lm

# include paths
INCS = -I$(MOD_HOST_DIR)/src $(CLAP_CFLAGS) $(shell pkg-config --cflags jack)

LDFLAGS += -Wl,--no-undefined

# source and object files
SRC = src/main.c src/effects.c src/clap_host.c
OBJ = $(SRC:.c=.o)

# default build
all: $(PROG)

# linking rule
$(PROG): $(OBJ) $(PLUMBING_LIB)
	$(CC) $(OBJ) $(PLUMBING_LIB) $(LDFLAGS) $(LIBS) -o $@

$(PLUMBING_LIB):
	$(MAKE) -C $(MOD_HOST_DIR) libmod-host-plumbing.a

# meta-rule to generate the object files
%.o: %.c
	$(CC) $(INCS) $(CFLAGS) -c -o $@ $<

# install rule
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

install:
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(PROG) $(DESTDIR)$(BINDIR)

# clean rule
clean:
	@rm -f src/*.o $(PROG) tests/clap_host_test

# the CLAP lifecycle against a plugin, no jack needed
test: tests/clap_host_test
	./tests/clap_host_test $(CLAP_TEST_PLUGIN)

tests/clap_host_test: tests/clap_host_test.c src/clap_host.c
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $^ -ldl -lpthread -lm
