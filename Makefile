# compiler
CC ?= gcc

# program names
PROG = omx-clap-host
SCAN_PROG = omx-clap-scan

PKG_CONFIG ?= pkg-config

# mod-host protocol library: pkg-config when it is installed, a mod-host checkout otherwise
ifeq ($(shell $(PKG_CONFIG) --exists mod-host-protocol && echo true), true)
PROTOCOL_CFLAGS = $(shell $(PKG_CONFIG) --cflags mod-host-protocol)
PROTOCOL_LIBS = $(shell $(PKG_CONFIG) --libs mod-host-protocol)
else
MOD_HOST_DIR ?= ../wt-mod-host-clap
PROTOCOL_LIB = $(MOD_HOST_DIR)/libmod-host-protocol.so
PROTOCOL_CFLAGS = -I$(MOD_HOST_DIR)/src
# the checkout's library is not installed: the binary finds it there through its rpath
PROTOCOL_LIBS = -L$(MOD_HOST_DIR) -lmod-host-protocol -Wl,-rpath,$(abspath $(MOD_HOST_DIR))
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
INCS = $(PROTOCOL_CFLAGS) $(CLAP_CFLAGS) $(shell $(PKG_CONFIG) --cflags jack)

LDFLAGS += -Wl,--no-undefined

# source and object files
SRC = src/main.c src/effects.c src/clap_host.c
OBJ = $(SRC:.c=.o)

# the scanner shares the plugin loading with the host and needs neither jack nor the protocol library
SCAN_SRC = src/scan.c src/clap_host.c
SCAN_OBJ = $(SCAN_SRC:.c=.o)

# default build
all: $(PROG) $(SCAN_PROG)

# linking rule
$(PROG): $(OBJ) $(PROTOCOL_LIB)
	$(CC) $(OBJ) $(PROTOCOL_LIBS) $(LDFLAGS) $(LIBS) -o $@

ifneq ($(PROTOCOL_LIB),)
$(PROTOCOL_LIB):
	$(MAKE) -C $(MOD_HOST_DIR) libmod-host-protocol.so
endif

$(SCAN_PROG): $(SCAN_OBJ)
	$(CC) $(SCAN_OBJ) $(LDFLAGS) -ldl -lpthread -lm -o $@

# meta-rule to generate the object files
%.o: %.c
	$(CC) $(INCS) $(CFLAGS) -c -o $@ $<

# install rule
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1

install: install_man
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(PROG) $(SCAN_PROG) $(DESTDIR)$(BINDIR)

install_man:
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 doc/*.1 $(DESTDIR)$(MANDIR)

# clean rule
clean:
	@rm -f src/*.o $(PROG) $(SCAN_PROG) tests/clap_host_test tests/clap_scan_test tests/fake.clap tests/crash.clap tests/jack_latency_probe tests/jack_identity

# the CLAP lifecycle against a plugin, no jack needed; the layouts the host refuses come from a fake .clap
test: tests/clap_host_test tests/fake.clap test-scan
	./tests/clap_host_test $(CLAP_TEST_PLUGIN) $(abspath tests/fake.clap)

# the scanner against the fake plugin, one that crashes, a broken file, a directory walk and omx-delay.clap
test-scan: $(SCAN_PROG) tests/clap_scan_test tests/fake.clap tests/crash.clap
	./tests/clap_scan_test ./$(SCAN_PROG) $(abspath tests/fake.clap) $(abspath tests/crash.clap) $(CLAP_TEST_PLUGIN)

# the same without omx-delay.clap: only the fake plugin's checks
test-fake: tests/clap_host_test tests/fake.clap $(SCAN_PROG) tests/clap_scan_test tests/crash.clap
	./tests/clap_host_test - $(abspath tests/fake.clap)
	./tests/clap_scan_test ./$(SCAN_PROG) $(abspath tests/fake.clap) $(abspath tests/crash.clap)

tests/clap_host_test: tests/clap_host_test.c src/clap_host.c
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $^ -ldl -lpthread -lm

tests/fake.clap: tests/fake_plugin.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/crash.clap: tests/crash_plugin.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/clap_scan_test: tests/clap_scan_test.c
	$(CC) $(CFLAGS) -Werror -o $@ $<

# the host over jack, in a PipeWire of its own: every command on the wire and the ports it makes
test-jack: $(PROG) tests/fake.clap tests/jack_latency_probe
	CLAP_TEST_PLUGIN=$(CLAP_TEST_PLUGIN) ./tests/jack_e2e.sh

tests/jack_latency_probe: tests/jack_latency_probe.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack)

# the LV2 twin through mod-host against the CLAP twin through this host, bit for bit, in a PipeWire of its own
test-identity: $(PROG) tests/jack_identity
	CLAP_TEST_PLUGIN=$(CLAP_TEST_PLUGIN) ./tests/clap_lv2_identity.sh

tests/jack_identity: tests/jack_identity.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack) -lm
