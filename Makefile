# compiler
CC ?= gcc

# program names
PROG = omx-clap-host
SCAN_PROG = omx-clap-scan

# the hosting core, a shared library of its own: soname libomx-clap-core.so.<major>, the file <major>.<minor>.<patch>
CORE = omx-clap-core
CORE_MAJOR = 0
CORE_VERSION = 0.1.0
CORE_SO = lib$(CORE).so
CORE_SONAME = $(CORE_SO).$(CORE_MAJOR)
CORE_FILE = $(CORE_SO).$(CORE_VERSION)
CORE_HEADERS = src/clap_host.h src/clap_stage.h src/hosted_stage.h src/clap_host_limits.h
CORE_MAP = src/omx-clap-core.map

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

# default compiler and linker flags; everything is hidden by default, the core's export list is its version script
CFLAGS += -O3 -g -Wall -Wextra -std=gnu99 -fPIC -fvisibility=hidden -D_GNU_SOURCE -pthread -MMD -MP
CFLAGS += -Werror=implicit-function-declaration -Werror=return-type

ifeq ($(DEBUG), 1)
   CFLAGS += -O0 -DDEBUG
else
   LDFLAGS += -s
endif

# where the programs find the core: the build tree's, unless a package builds them for the system's
RPATH ?= -Wl,-rpath,$(CURDIR)
# the programs a test runs find the core in the build tree even when a package built them without an rpath
export LD_LIBRARY_PATH := $(CURDIR)$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH))
CORE_LINK = -L. -l$(CORE) $(RPATH)
CORE_LINK_TEST = -L. -l$(CORE) -Wl,-rpath,$(CURDIR)

# libraries
LIBS = $(shell $(PKG_CONFIG) --libs jack 2>/dev/null) -ldl -lpthread -lm

# include paths
INCS = $(PROTOCOL_CFLAGS) $(CLAP_CFLAGS) $(shell $(PKG_CONFIG) --cflags jack 2>/dev/null)

LDFLAGS += -Wl,--no-undefined

# source and object files
SRC = src/main.c src/effects.c
OBJ = $(SRC:.c=.o)
CORE_SRC = src/clap_host.c
CORE_OBJ = $(CORE_SRC:.c=.o)

# the scanner shares the plugin loading with the host through the core and needs neither jack nor the protocol library
SCAN_SRC = src/scan.c
SCAN_OBJ = $(SCAN_SRC:.c=.o)

# default build
all: $(PROG) $(SCAN_PROG)

# the core: names no jack, no socket and nothing of the protocol library
$(CORE_FILE): $(CORE_OBJ) $(CORE_MAP)
	$(CC) -shared -Wl,-soname,$(CORE_SONAME) -Wl,--version-script=$(CORE_MAP) -Wl,--no-undefined $(CORE_OBJ) -ldl -lpthread -lm -o $@

$(CORE_SONAME): $(CORE_FILE)
	ln -sf $< $@

$(CORE_SO): $(CORE_SONAME)
	ln -sf $< $@

# linking rule
$(PROG): $(OBJ) $(CORE_SO) $(PROTOCOL_LIB)
	$(CC) $(OBJ) $(CORE_LINK) $(PROTOCOL_LIBS) $(LDFLAGS) $(LIBS) -o $@

ifneq ($(PROTOCOL_LIB),)
$(PROTOCOL_LIB):
	$(MAKE) -C $(MOD_HOST_DIR) libmod-host-protocol.so
endif

$(SCAN_PROG): $(SCAN_OBJ) $(CORE_SO)
	$(CC) $(SCAN_OBJ) $(CORE_LINK) $(LDFLAGS) -ldl -lpthread -lm -o $@

# meta-rule to generate the object files
%.o: %.c
	$(CC) $(INCS) $(CFLAGS) -c -o $@ $<

# install rule
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include
DATADIR = $(PREFIX)/share
MANDIR = $(DATADIR)/man/man1

# the library, its headers, the pkg-config file and the export list: what a program that hosts CLAP plugins builds against
install-lib: $(CORE_SO)
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig $(DESTDIR)$(INCLUDEDIR)/omx-clap-host $(DESTDIR)$(DATADIR)/$(CORE)
	install -m 755 $(CORE_FILE) $(DESTDIR)$(LIBDIR)/
	ln -sf $(CORE_FILE) $(DESTDIR)$(LIBDIR)/$(CORE_SONAME)
	ln -sf $(CORE_SONAME) $(DESTDIR)$(LIBDIR)/$(CORE_SO)
	install -m 644 $(CORE_HEADERS) $(DESTDIR)$(INCLUDEDIR)/omx-clap-host/
	sed -e 's,@PREFIX@,$(PREFIX),' -e 's,@LIBDIR@,$(LIBDIR),' -e 's,@INCLUDEDIR@,$(INCLUDEDIR),' \
	    -e 's,@DATADIR@,$(DATADIR),' -e 's,@VERSION@,$(CORE_VERSION),' omx-clap-core.pc.in > $(DESTDIR)$(LIBDIR)/pkgconfig/omx-clap-core.pc
	chmod 644 $(DESTDIR)$(LIBDIR)/pkgconfig/omx-clap-core.pc
	install -m 644 $(CORE_MAP) $(DESTDIR)$(DATADIR)/$(CORE)/
	if [ -f abi/$(CORE_SONAME).abi ]; then install -m 644 abi/$(CORE_SONAME).abi $(DESTDIR)$(DATADIR)/$(CORE)/; fi

install: install-lib install_man
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(PROG) $(SCAN_PROG) $(DESTDIR)$(BINDIR)

install_man:
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 doc/*.1 $(DESTDIR)$(MANDIR)

# clean rule
clean:
	@rm -rf src/*.o src/*.d tests/*.d $(PROG) $(SCAN_PROG) $(CORE_SO)* build tests/clap_host_test tests/core_link_test tests/clap_scan_test tests/fake.clap tests/fake_synth.clap tests/crash.clap tests/jack_latency_probe tests/jack_synth_probe tests/jack_identity

-include $(wildcard src/*.d)

# the CLAP lifecycle against a plugin, no jack needed; the layouts the host refuses come from a fake .clap
test: tests/clap_host_test tests/fake.clap tests/fake_synth.clap test-scan test-core
	./tests/clap_host_test $(CLAP_TEST_PLUGIN) $(abspath tests/fake.clap) $(abspath tests/fake_synth.clap)

# the scanner against the fake plugin, one that crashes, a broken file, a directory walk, omx-delay.clap and the fake synth
test-scan: $(SCAN_PROG) tests/clap_scan_test tests/fake.clap tests/crash.clap tests/fake_synth.clap
	./tests/clap_scan_test ./$(SCAN_PROG) $(abspath tests/fake.clap) $(abspath tests/crash.clap) $(CLAP_TEST_PLUGIN) $(abspath tests/fake_synth.clap)

# the same without omx-delay.clap: only the fake plugin's checks
test-fake: tests/clap_host_test tests/fake.clap tests/fake_synth.clap $(SCAN_PROG) tests/clap_scan_test tests/crash.clap test-core
	./tests/clap_host_test - $(abspath tests/fake.clap) $(abspath tests/fake_synth.clap)
	./tests/clap_scan_test ./$(SCAN_PROG) $(abspath tests/fake.clap) $(abspath tests/crash.clap) - $(abspath tests/fake_synth.clap)

tests/clap_host_test: tests/clap_host_test.c $(CORE_SO)
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $< $(CORE_LINK_TEST) -lpthread -lm

# the ABI: abidw records what the library exports and the types its public headers reach, abidiff compares a build with the
# baseline of the last release. An addition is a compatible change and a new minor; a removal or a change is a new soname major.
ABI_BASELINE = abi/$(CORE_SONAME).abi

abi-stage: $(CORE_SO)
	rm -rf build/abi
	$(MAKE) install-lib DESTDIR=$(CURDIR)/build/abi PREFIX=/usr LIBDIR=/usr/lib

# record the baseline: the release commit does this and commits abi/
abi-baseline: abi-stage
	mkdir -p abi
	abidw --headers-dir build/abi/usr/include --out-file $(ABI_BASELINE) build/abi/usr/lib/$(CORE_FILE)

abi-check: abi-stage
	sh tests/abi-check.sh build/abi/usr/lib/$(CORE_FILE) build/abi/usr/include $(ABI_BASELINE)

# the library as a program outside this tree sees it: installed into a prefix of its own, found through its pkg-config file
# and linked by that alone, with the console's defaults; the export list and the libraries it names are read off the file
test-core: tests/core_link_test tests/fake.clap tests/fake_synth.clap $(CORE_SO)
	sh tests/exports.sh $(CORE_FILE) $(CORE_MAP) src/clap_host.h
	for h in $(CORE_HEADERS); do echo "#include \"$$(basename $$h)\"" | $(CC) -x c -fsyntax-only -Wall -Wextra -Werror -std=gnu99 -Isrc $(CLAP_CFLAGS) - || exit 1; done; echo "ok   each installed header compiles on its own"
	./tests/core_link_test $(abspath tests/fake.clap) $(abspath tests/fake_synth.clap)

tests/core_link_test: tests/core_link_test.c $(CORE_SO) omx-clap-core.pc.in
	rm -rf build/stage
	$(MAKE) install-lib DESTDIR=$(CURDIR)/build/stage PREFIX=/usr LIBDIR=/usr/lib
	sed -i 's,^prefix=.*,prefix=$(CURDIR)/build/stage/usr,' build/stage/usr/lib/pkgconfig/omx-clap-core.pc
	sed -i 's,^libdir=.*,libdir=$${prefix}/lib,; s,^includedir=.*,includedir=$${prefix}/include,' build/stage/usr/lib/pkgconfig/omx-clap-core.pc
	$(PKG_CONFIG) --exists clap || printf 'Name: clap\nDescription: the headers named by CLAP_CFLAGS\nVersion: 1\nCflags: $(CLAP_CFLAGS)\n' > build/stage/usr/lib/pkgconfig/clap.pc
	export PKG_CONFIG_PATH=$(CURDIR)/build/stage/usr/lib/pkgconfig; $(CC) -O2 -Wall -Wextra -Werror -std=gnu99 -D_GNU_SOURCE -o $@ $< \
	    $$($(PKG_CONFIG) --cflags --libs omx-clap-core) -Wl,-rpath,$(CURDIR)/build/stage/usr/lib -lpthread -lm

tests/fake.clap: tests/fake_plugin.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/fake_synth.clap: tests/fake_synth.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $< -lm

tests/crash.clap: tests/crash_plugin.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/clap_scan_test: tests/clap_scan_test.c
	$(CC) $(CFLAGS) -Werror -o $@ $<

# the host over jack, in a PipeWire of its own: every command on the wire and the ports it makes
test-jack: $(PROG) tests/fake.clap tests/jack_latency_probe
	CLAP_TEST_PLUGIN=$(CLAP_TEST_PLUGIN) ./tests/jack_e2e.sh

tests/jack_latency_probe: tests/jack_latency_probe.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack)

# an instrument over jack in the same kind of namespace: MIDI into midi_in, the level that comes out, the silence after the note off
test-jack-synth: $(PROG) tests/fake_synth.clap tests/jack_synth_probe
	./tests/jack_synth_e2e.sh

tests/jack_synth_probe: tests/jack_synth_probe.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack) -lm

# the LV2 twin through mod-host against the CLAP twin through this host, bit for bit, in a PipeWire of its own
test-identity: $(PROG) tests/jack_identity
	CLAP_TEST_PLUGIN=$(CLAP_TEST_PLUGIN) ./tests/clap_lv2_identity.sh

tests/jack_identity: tests/jack_identity.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack) -lm
