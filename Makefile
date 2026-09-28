# compiler
CC ?= gcc

# program name
PROG = omx-clap-host

PKG_CONFIG ?= pkg-config

# mod-host plumbing library: pkg-config when it is installed, a mod-host checkout otherwise
ifeq ($(shell $(PKG_CONFIG) --exists mod-host-plumbing && echo true), true)
PLUMBING_CFLAGS = $(shell $(PKG_CONFIG) --cflags mod-host-plumbing)
PLUMBING_LIBS = $(shell $(PKG_CONFIG) --libs mod-host-plumbing)
else
MOD_HOST_DIR ?= ../wt-mod-host-clap
PLUMBING_LIB = $(MOD_HOST_DIR)/libmod-host-plumbing.a
PLUMBING_CFLAGS = -I$(MOD_HOST_DIR)/src
PLUMBING_LIBS = $(PLUMBING_LIB)
endif

# CLAP headers: pkg-config when clap-devel is installed, CLAP_CFLAGS=-I<dir> otherwise
CLAP_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags clap 2>/dev/null)

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
LIBS = $(shell $(PKG_CONFIG) --libs jack) -ldl -lpthread -lm

# include paths
INCS = $(PLUMBING_CFLAGS) $(CLAP_CFLAGS) $(shell $(PKG_CONFIG) --cflags jack)

LDFLAGS += -Wl,--no-undefined

# source and object files
SRC = src/main.c src/effects.c src/clap_host.c
OBJ = $(SRC:.c=.o)

# default build
all: $(PROG)

# linking rule
$(PROG): $(OBJ) $(PLUMBING_LIB)
	$(CC) $(OBJ) $(PLUMBING_LIBS) $(LDFLAGS) $(LIBS) -o $@

ifneq ($(PLUMBING_LIB),)
$(PLUMBING_LIB):
	$(MAKE) -C $(MOD_HOST_DIR) libmod-host-plumbing.a
endif

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
	@rm -f src/*.o $(PROG) tests/clap_host_test tests/fake.clap

# the CLAP lifecycle against a plugin, no jack needed; the layouts the host refuses come from a fake .clap
test: tests/clap_host_test tests/fake.clap
	./tests/clap_host_test $(CLAP_TEST_PLUGIN) $(abspath tests/fake.clap)

tests/clap_host_test: tests/clap_host_test.c src/clap_host.c
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $^ -ldl -lpthread -lm

tests/fake.clap: tests/fake_plugin.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<
