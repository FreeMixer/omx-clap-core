# SPDX-License-Identifier: GPL-3.0-or-later
# compiler
CC ?= gcc

# the one version of the library, the adapter and the programs: VERSION holds it, and the spec's Version, debian/changelog
# and the release notes are written from CHANGELOG.md, whose newest entry `make version-check` holds to it
VERSION := $(strip $(shell cat VERSION))
# the library reports it as major * 10000 + minor * 100 + patch (omx_clap_core_version())
VERSION_NUM := $(shell echo $(VERSION) | awk -F. '{ print $$1 * 10000 + $$2 * 100 + $$3 }')

# program names
PROG = omx-clap-host
SCAN_PROG = omx-clap-scan

# the hosting core, a shared library of its own: soname libomx-clap-core.so.<major>, the file <major>.<minor>.<patch>
CORE = omx-clap-core
CORE_MAJOR = 0
CORE_VERSION = $(VERSION)
CORE_SO = lib$(CORE).so
CORE_SONAME = $(CORE_SO).$(CORE_MAJOR)
CORE_FILE = $(CORE_SO).$(CORE_VERSION)
CORE_HEADERS = src/clap_host.h src/clap_host_extensions.h src/omx_msgring.h src/clap_stage.h src/hosted_stage.h src/clap_host_limits.h src/omx_clap_ext.h
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

# plugin-hostd's protocol and pin headers: pkg-config when plugin-hostd-devel is installed, a plugin-hostd checkout otherwise
PLUGIN_HOSTD_DIR ?= ../plugin-hostd
ifeq ($(shell $(PKG_CONFIG) --exists plugin-hostd && echo true), true)
PLUGIN_HOSTD_CFLAGS = $(shell $(PKG_CONFIG) --cflags plugin-hostd)
else
PLUGIN_HOSTD_CFLAGS = -I$(PLUGIN_HOSTD_DIR)/include
endif

# CLAP headers: pkg-config when clap-devel is installed, CLAP_CFLAGS=-I<dir> otherwise
CLAP_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags clap 2>/dev/null)

# plugin the test loads
CLAP_TEST_PLUGIN ?= ../openmixer/packages/omx-plugins/bin/omx-delay.clap

# default compiler and linker flags; everything is hidden by default, the core's export list is its version script
CFLAGS += -O3 -g -Wall -Wextra -std=gnu99 -fPIC -fvisibility=hidden -D_GNU_SOURCE -pthread -MMD -MP
CFLAGS += -Werror=implicit-function-declaration -Werror=return-type
CFLAGS += -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u

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
INCS = $(PROTOCOL_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) $(CLAP_CFLAGS) $(shell $(PKG_CONFIG) --cflags jack 2>/dev/null)

LDFLAGS += -Wl,--no-undefined

# source and object files
SRC = src/main.c src/effects.c
OBJ = $(SRC:.c=.o)
CORE_SRC = src/clap_host.c
CORE_OBJ = $(CORE_SRC:.c=.o)

# the scanner shares the plugin loading with the host through the core and needs neither jack nor the protocol library
SCAN_SRC = src/scan.c
SCAN_OBJ = $(SCAN_SRC:.c=.o)

# the LV2 adapter, libomx-clap-lv2.a: lv2/core is the LV2 host with no CLAP in it, lv2/clap the shim that presents one LV2
# plugin as one CLAP plugin. A static archive, position-independent so a shared object can link it; lilv is never linked,
# only its headers are read. lv2/core's own archive is internal: its tests link it, nothing installs it.
LV2_LIB = libomx-clap-lv2.a
LV2_VERSION = $(VERSION)
LV2_CORE_LIB = build/lv2/liblv2core.a
LV2_HEADERS = lv2/clap/omx_clap_lv2.h
LV2_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags lv2 lilv-0 2>/dev/null)
LV2_CORE_SRC = lv2/core/lv2_world.c lv2/core/lv2_features.c lv2/core/lv2_instance.c
LV2_CORE_OBJ = $(LV2_CORE_SRC:.c=.o)
LV2_CLAP_SRC = lv2/clap/lv2_clap.c
LV2_CLAP_OBJ = $(LV2_CLAP_SRC:.c=.o)
# the lilv the tests' lazy-load arm points a late symlink at
LILV_LIB ?= $(shell $(PKG_CONFIG) --variable=libdir lilv-0 2>/dev/null)/liblilv-0.so.0

# default build
all: $(PROG) $(SCAN_PROG) $(LV2_LIB)

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

# lv2/core names no CLAP header: its include path holds no CLAP directory
lv2/core/%.o: lv2/core/%.c
	$(CC) -Isrc -Ilv2/core $(LV2_CFLAGS) $(CFLAGS) -c -o $@ $<

lv2/clap/%.o: lv2/clap/%.c
	$(CC) -Isrc -Ilv2/core -Ilv2/clap $(LV2_CFLAGS) $(CLAP_CFLAGS) $(CFLAGS) -c -o $@ $<

$(LV2_CORE_LIB): $(LV2_CORE_OBJ)
	@mkdir -p build/lv2
	rm -f $@
	ar rcs $@ $^

$(LV2_LIB): $(LV2_CORE_OBJ) $(LV2_CLAP_OBJ)
	rm -f $@
	ar rcs $@ $^

.PHONY: lv2
lv2: $(LV2_LIB)

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

# the LV2 adapter: the archive, its one header and its pkg-config file, what a program that hosts LV2 plugins as CLAP
# plugins builds against; lilv is the program's to have at run time (the adapter dlopens liblilv-0.so.0)
install-lv2: $(LV2_LIB)
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig $(DESTDIR)$(INCLUDEDIR)/omx-clap-lv2
	install -m 644 $(LV2_LIB) $(DESTDIR)$(LIBDIR)/
	install -m 644 $(LV2_HEADERS) $(DESTDIR)$(INCLUDEDIR)/omx-clap-lv2/
	sed -e 's,@PREFIX@,$(PREFIX),' -e 's,@LIBDIR@,$(LIBDIR),' -e 's,@INCLUDEDIR@,$(INCLUDEDIR),' \
	    -e 's,@VERSION@,$(LV2_VERSION),' omx-clap-lv2.pc.in > $(DESTDIR)$(LIBDIR)/pkgconfig/omx-clap-lv2.pc
	chmod 644 $(DESTDIR)$(LIBDIR)/pkgconfig/omx-clap-lv2.pc

install: install-lib install-lv2 install_man
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(PROG) $(SCAN_PROG) $(DESTDIR)$(BINDIR)

install_man:
	install -d $(DESTDIR)$(MANDIR)
	for page in doc/*.1; do sed -e 's,@VERSION@,$(VERSION),' $$page > $(DESTDIR)$(MANDIR)/$$(basename $$page); chmod 644 $(DESTDIR)$(MANDIR)/$$(basename $$page); done

# the version of this tree is the newest entry of CHANGELOG.md: the spec and debian/changelog are written from that file
.PHONY: version-check print-version-num
print-version-num:
	@echo $(VERSION_NUM)

version-check:
	@newest=$$(sed -n 's/^## \([0-9][0-9.]*\)\(-[0-9]*\)\{0,1\} - .*/\1/p' CHANGELOG.md | head -1); \
	if [ "$$newest" = "$(VERSION)" ]; then echo "ok   VERSION $(VERSION) is the newest entry of CHANGELOG.md"; \
	else echo "FAIL VERSION says $(VERSION), the newest entry of CHANGELOG.md says $$newest"; exit 1; fi

# clean rule
.PHONY: docs
# the C API reference by doxygen, from the installed headers' docstrings; build-time only
docs: $(CORE_HEADERS) Doxyfile
	mkdir -p build
	doxygen Doxyfile

clean:
	@rm -rf src/*.o src/*.d tests/*.d lv2/*/*.o lv2/*/*.d $(LV2_LIB) lv2/tests/lv2_run_test lv2/tests/lv2_host_test lv2/tests/lv2_clap_test lv2/tests/lv2_cost lv2/tests/lv2_link_test $(PROG) $(SCAN_PROG) $(CORE_SO)* build tests/clap_host_test tests/core_link_test tests/clap_scan_test tests/clap_stage_test tests/clap_core_test tests/clap_untrusted_test tests/clap_untrusted_asan tests/msgring_test tests/fault-*.clap tests/fake.clap tests/fake_synth.clap tests/fake_compressor.clap tests/crash.clap tests/jack_latency_probe tests/jack_synth_probe tests/jack_meter_source tests/jack_identity tests/clap_layout_pin

-include $(wildcard src/*.d lv2/*/*.d)

# the CLAP lifecycle against a plugin, no jack needed; the layouts the host refuses come from a fake .clap
test: tests/clap_host_test tests/fake.clap tests/fake_synth.clap test-scan test-core
	./tests/clap_host_test $(CLAP_TEST_PLUGIN) $(abspath tests/fake.clap) $(abspath tests/fake_synth.clap)

# the scanner against the fake plugin, one that crashes, a broken file, a directory walk, omx-delay.clap and the fake synth
test-scan: $(SCAN_PROG) tests/clap_scan_test tests/fake.clap tests/crash.clap tests/fake_synth.clap
	./tests/clap_scan_test ./$(SCAN_PROG) $(abspath tests/fake.clap) $(abspath tests/crash.clap) $(CLAP_TEST_PLUGIN) $(abspath tests/fake_synth.clap)

# the same without omx-delay.clap: only the fake plugin's checks
test-fake: tests/clap_host_test tests/fake.clap tests/fake_synth.clap $(SCAN_PROG) tests/clap_scan_test tests/crash.clap test-core test-stage test-msgring
	./tests/clap_host_test - $(abspath tests/fake.clap) $(abspath tests/fake_synth.clap)
	./tests/clap_scan_test ./$(SCAN_PROG) $(abspath tests/fake.clap) $(abspath tests/crash.clap) - $(abspath tests/fake_synth.clap)

# the stage against fake plugins at six rates, and the core over the fault fixtures and fakes: the witness, the guard
# page, the audio role on a split, the whole roster and the tempo
test-stage: tests/clap_stage_test tests/clap_core_test tests/clap_untrusted_test fixtures
	./tests/clap_stage_test
	./tests/clap_core_test $(abspath tests)
	./tests/clap_untrusted_test

# the parameters a plugin reports, with the core compiled into the test under AddressSanitizer: a read past the plugin's
# own buffers is an error here, not luck. Needs libasan; CI runs it, the package builds do not
test-untrusted-asan: tests/clap_untrusted_asan
	ASAN_OPTIONS=detect_leaks=0:check_printf=1 ./tests/clap_untrusted_asan

# the worker message ring of the shared host-services layer, single- and two-threaded
test-msgring: tests/msgring_test
	./tests/msgring_test

tests/msgring_test: tests/msgring_test.c src/omx_msgring.h
	$(CC) $(CFLAGS) -Werror -pthread -o $@ $<

# --wrap sees no call that link-time optimisation has already resolved inside one unit: the wrapped tests build without it
WRAP_ALLOC = -fno-lto -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

tests/clap_stage_test: tests/clap_stage_test.c src/clap_stage.h src/hosted_stage.h
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror $(WRAP_ALLOC) -o $@ $< -lm

tests/clap_core_test: tests/clap_core_test.c $(CORE_SO)
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror $(WRAP_ALLOC) -o $@ $< $(CORE_LINK_TEST) -lpthread -lm

# defines calloc to fail one size on demand, which the core links from it: no --wrap, no link-time optimisation, no builtin
tests/clap_untrusted_test: tests/clap_untrusted_test.c src/layout_pin.h $(CORE_SO)
	$(CC) $(CLAP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) $(CFLAGS) -Werror -fno-lto -fno-builtin-calloc -o $@ $< $(CORE_LINK_TEST) -lpthread -lm

tests/clap_untrusted_asan: tests/clap_untrusted_test.c $(CORE_SRC) $(CORE_HEADERS)
	$(CC) -Isrc $(CLAP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) -O1 -g -std=gnu99 -D_GNU_SOURCE -pthread -Wall -Wextra -Werror -fno-omit-frame-pointer \
	    -fsanitize=address,undefined -fno-sanitize-recover=all -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u -DOMX_UNTRUSTED_ASAN -o $@ $< $(CORE_SRC) -ldl -lpthread -lm

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
# and linked by that alone, with the defaults; the export list and the libraries it names are read off the file
test-core: tests/core_link_test tests/fake.clap tests/fake_synth.clap $(CORE_SO)
	sh tests/exports.sh $(CORE_FILE) $(CORE_MAP) src/clap_host.h
	for h in $(CORE_HEADERS); do echo "#include \"$$(basename $$h)\"" | $(CC) -x c -fsyntax-only -Wall -Wextra -Werror -std=gnu99 -Isrc $(CLAP_CFLAGS) - || exit 1; done; echo "ok   each installed header compiles on its own"
	@echo "the RT headers reach nothing that loads, allocates or waits:"; ! grep -n -E '#include <(dlfcn|pthread|stdlib|unistd|stdio)\.h>|clap_entry|dlopen|malloc|calloc' src/hosted_stage.h src/clap_stage.h src/omx_msgring.h && echo "ok   hosted_stage.h, clap_stage.h and omx_msgring.h include none of dlfcn, pthread, stdlib, unistd, stdio and name no clap_entry, dlopen or allocator"
	@echo "clap_host.h's inline RT code calls nothing that allocates, waits, locks or prints:"; mkdir -p build && sed -n '/INLINE FUNCTIONS/,$$p' src/clap_host.h > build/clap_host_inline.h && grep -q 'omx_clap_host_run(' build/clap_host_inline.h && ! grep -n -E '\<(malloc|calloc|realloc|free|usleep|nanosleep|sleep|dlopen|dlsym|printf|fprintf|snprintf|puts)\s*\(|pthread_|_lock\s*\(' build/clap_host_inline.h && echo "ok   the inline section of clap_host.h (omx_clap_host_run and the tempo word) names no allocator, sleep, lock, loader or printf"
	./tests/core_link_test $(abspath tests/fake.clap) $(abspath tests/fake_synth.clap)

tests/core_link_test: tests/core_link_test.c $(CORE_SO) omx-clap-core.pc.in
	rm -rf build/stage
	$(MAKE) install-lib PREFIX=$(CURDIR)/build/stage/usr LIBDIR=$(CURDIR)/build/stage/usr/lib
	$(PKG_CONFIG) --exists clap || printf 'Name: clap\nDescription: the headers named by CLAP_CFLAGS\nVersion: 1\nCflags: $(CLAP_CFLAGS)\n' > build/stage/usr/lib/pkgconfig/clap.pc
	export PKG_CONFIG_PATH=$(CURDIR)/build/stage/usr/lib/pkgconfig; $(CC) -O2 -Wall -Wextra -Werror -std=gnu99 -D_GNU_SOURCE -DOMX_EXPECT_VERSION_NUM=$(VERSION_NUM)u -o $@ $< \
	    $$($(PKG_CONFIG) --cflags --libs omx-clap-core) -Wl,-rpath,$(CURDIR)/build/stage/usr/lib -lpthread -lm

# what one stage block costs, as the header is and with omx_clap_run's two in_cycle stores relaxed: a measurement
bench-stage: tests/stage_bench.c src/clap_stage.h src/hosted_stage.h
	mkdir -p build/bench-relaxed
	sed -e 's/atomic_store(&s->in_cycle, \([01]\));/atomic_store_explicit(\&s->in_cycle, \1, memory_order_relaxed);/' src/clap_stage.h > build/bench-relaxed/clap_stage.h
	test $$(grep -c 'in_cycle, [01], memory_order_relaxed' build/bench-relaxed/clap_stage.h) -eq 2
	cp src/hosted_stage.h src/clap_host_limits.h build/bench-relaxed/
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror -DBENCH_LABEL='"seq_cst"' -o build/stage_bench_seq_cst $< -lm
	$(CC) -Ibuild/bench-relaxed $(CLAP_CFLAGS) $(CFLAGS) -Werror -DBENCH_LABEL='"relaxed"' -o build/stage_bench_relaxed $< -lm
	for i in 1 2 3; do ./build/stage_bench_seq_cst; ./build/stage_bench_relaxed; done

# the fault fixtures, one .clap per mode of tests/fault_clap.c: also what a consumer's tests load
FAULT_MODES = 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16
FIXTURES = $(foreach m,$(FAULT_MODES),tests/fault-$(m).clap)
fixtures: $(FIXTURES)

tests/fault-%.clap: tests/fault_clap.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -DOMX_FAULT_MODE=$* -o $@ $< -lm

tests/fake.clap: tests/fake_plugin.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/fake_synth.clap: tests/fake_synth.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $< -lm

tests/fake_compressor.clap: tests/fake_compressor.c src/omx_clap_ext.h
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $< -lm

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

# the meters over jack in the same kind of namespace: monitor_output on each derived symbol and the output_set lines the
# feedback socket carries, a constant input fed by jack_meter_source
test-jack-meters: $(PROG) tests/fake_compressor.clap tests/fake.clap tests/jack_meter_source
	./tests/jack_meters_e2e.sh

# layout pinning over jack in the same kind of namespace: pin_expect before add, a layout that matches loads, one that
# differs is destroyed before activate with nothing processed, a scheme this host does not know is refused
test-jack-pin: $(PROG) tests/fake.clap tests/clap_layout_pin
	$(PHD_DECLARED) ./tests/jack_pin_e2e.sh

# track_info, remote pages and param_info over jack in the same kind of namespace: what the plugin's clap.track-info
# reads, its two remote pages, remote_pages_changed on the feedback socket, every call on the thread that ran init
test-jack-info: $(PROG) tests/fake.clap
	$(PHD_DECLARED) ./tests/jack_info_e2e.sh

tests/clap_layout_pin: tests/clap_layout_pin.c src/layout_pin.h
	$(CC) -Isrc $(PLUGIN_HOSTD_CFLAGS) $(CLAP_CFLAGS) $(CFLAGS) -Werror -o $@ $< -ldl

# the same behind plugin-hostd: the daemon hashes the .clap, sends pin_expect with the layout pin_set gave it, and this
# host answers the add. PLUGIN_HOSTD is the daemon, built in a plugin-hostd checkout by default.
PLUGIN_HOSTD ?= $(PLUGIN_HOSTD_DIR)/plugin-hostd
test-hostd-pin: $(PROG) tests/fake.clap tests/clap_layout_pin
	$(PHD_DECLARED) PLUGIN_HOSTD=$(PLUGIN_HOSTD) ./tests/hostd_pin_e2e.sh

# what the pin tests read of plugin-hostd's protocol, from its header: a test names no code or verb of its own
HASH := \#
phd_declared = $(shell printf '$(HASH)include <plugin-hostd/protocol.h>\n$(HASH)include <plugin-hostd/pin.h>\n%s\n' $(1) | $(CC) $(PLUGIN_HOSTD_CFLAGS) -E -P - | tail -n 1 | sed -e 's/^[("]//' -e 's/[)"]$$//')
PHD_DECLARED = $(foreach n,PHD_ERR_PIN_ABSENT PHD_ERR_PIN_BINARY_MISMATCH PHD_ERR_PIN_LAYOUT_MISMATCH PHD_VERB_PIN_EXPECT PHD_VERB_PIN_SET PHD_VERB_PIN_CLEAR PHD_PIN_LAYOUT_SCHEME PHD_READY_LINE PHD_ERR_NO_PARAM_CONTRACT PHD_EVENT_REMOTE_PAGES_CHANGED,$(n)='$(call phd_declared,$(n))')

tests/jack_meter_source: tests/jack_meter_source.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack)

# the LV2 twin through mod-host against the CLAP twin through this host, bit for bit, in a PipeWire of its own
test-identity: $(PROG) tests/jack_identity
	CLAP_TEST_PLUGIN=$(CLAP_TEST_PLUGIN) ./tests/clap_lv2_identity.sh

tests/jack_identity: tests/jack_identity.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack) -lm

# ---- the LV2 adapter's tests ----

# the fixture bundles, built into build/lv2: their TTL beside a binary of their own, every symbol a test reads exported
LV2_FIXTURE_DIR = lv2/tests/fixtures
# -ffp-contract=off on the fixtures and the tests: an exact oracle (out == in * gain) holds only when neither side
# fuses a multiply into an add; aarch64's GCC fuses by default and the product's rounding then reads as an error
LV2_FIXTURE_CFLAGS = -O2 -g -Wall -Wextra -Werror -std=gnu99 -fPIC -shared -D_GNU_SOURCE -pthread -ffp-contract=off $(LV2_CFLAGS)
LV2_BUNDLES = build/lv2/omx-host-fixture.lv2/omx-host-fixture.so build/lv2/omx-worker-gain.lv2/omx-worker-gain.so \
              build/lv2/omx-lv2-fakes.lv2/omx-lv2-fakes.so

build/lv2/omx-host-fixture.lv2/omx-host-fixture.so: $(LV2_FIXTURE_DIR)/lv2_host_fixture.c $(wildcard $(LV2_FIXTURE_DIR)/omx-host-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-host-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-worker-gain.lv2/omx-worker-gain.so: $(LV2_FIXTURE_DIR)/lv2_worker_gain.c $(LV2_FIXTURE_DIR)/lv2_worker_gain.h $(wildcard $(LV2_FIXTURE_DIR)/omx-worker-gain.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-worker-gain.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -DLV2_WORKER_GAIN_BUNDLE -o $@ $< -lm

build/lv2/omx-lv2-fakes.lv2/omx-lv2-fakes.so: $(LV2_FIXTURE_DIR)/lv2_fakes.c $(LV2_FIXTURE_DIR)/lv2_fakes.h $(wildcard $(LV2_FIXTURE_DIR)/omx-lv2-fakes.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-lv2-fakes.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -fvisibility=hidden -o $@ $< -lm

LV2_TEST_CFLAGS = -Isrc -Ilv2/core -Ilv2/clap -Ilv2/tests $(LV2_CFLAGS) $(CFLAGS) -Werror -ffp-contract=off
LV2_WRAP = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

lv2/tests/lv2_run_test: lv2/tests/lv2_run_test.c $(LV2_CORE_LIB) lv2/tests/lv2_test_util.h $(LV2_FIXTURE_DIR)/lv2_fakes.h
	$(CC) $(LV2_TEST_CFLAGS) -o $@ $< $(LV2_CORE_LIB) $(LV2_WRAP) -ldl -lpthread -lm

lv2/tests/lv2_host_test: lv2/tests/lv2_host_test.c $(LV2_CORE_LIB) lv2/tests/lv2_test_util.h $(LV2_FIXTURE_DIR)/lv2_worker_gain.h
	$(CC) $(LV2_TEST_CFLAGS) -o $@ $< $(LV2_CORE_LIB) -ldl -lpthread -lm

# lv2/core: the run half against the console's fakes, the bundle half and the providers against real bundles; no lilv
# is linked into either test, and lv2/core names no CLAP header
test-lv2-core: lv2/tests/lv2_run_test lv2/tests/lv2_host_test $(LV2_BUNDLES)
	@! grep -n -E '#include [<"]clap' lv2/core/*.c lv2/core/*.h && echo "ok   lv2/core includes no CLAP header"
	@! readelf -d lv2/tests/lv2_run_test lv2/tests/lv2_host_test | grep -q 'liblilv' && echo "ok   no test links lilv: the host dlopens it"
	./lv2/tests/lv2_run_test build/lv2/omx-lv2-fakes.lv2
	./lv2/tests/lv2_host_test build/lv2 $(LILV_LIB)

.PHONY: test-lv2 test-lv2-core test-lv2-clap test-lv2-link install-lv2
test-lv2: test-lv2-core test-lv2-clap test-lv2-link test-lv2-validate

lv2/tests/lv2_clap_test: lv2/tests/lv2_clap_test.c $(LV2_LIB) $(CORE_SO) lv2/tests/lv2_test_util.h $(LV2_FIXTURE_DIR)/lv2_fakes.h
	$(CC) $(LV2_TEST_CFLAGS) $(CLAP_CFLAGS) -o $@ $< $(LV2_LIB) $(CORE_LINK_TEST) $(LV2_WRAP) -ldl -lpthread -lm

# lv2/clap: the CLAP face of the adapter, and the arms whose body is libomx-clap-core's run through it
test-lv2-clap: lv2/tests/lv2_clap_test $(LV2_BUNDLES)
	./lv2/tests/lv2_clap_test build/lv2

# the adapter's cost per process() call at 96 and 192 kHz, quantum 128: figures printed, nothing judged
lv2/tests/lv2_cost: lv2/tests/lv2_cost.c $(LV2_LIB) lv2/tests/lv2_test_util.h
	$(CC) $(LV2_TEST_CFLAGS) $(CLAP_CFLAGS) -o $@ $< $(LV2_LIB) -ldl -lpthread -lm

# the adapter as a program outside this tree sees it: installed into a prefix of its own, found through its pkg-config
# file and linked by that alone
lv2/tests/lv2_link_test: lv2/tests/lv2_link_test.c $(LV2_LIB) omx-clap-lv2.pc.in omx-clap-core.pc.in
	rm -rf build/lv2-stage
	$(MAKE) install-lib install-lv2 PREFIX=$(CURDIR)/build/lv2-stage/usr LIBDIR=$(CURDIR)/build/lv2-stage/usr/lib
	$(PKG_CONFIG) --exists clap || printf 'Name: clap\nDescription: the headers named by CLAP_CFLAGS\nVersion: 1\nCflags: $(CLAP_CFLAGS)\n' > build/lv2-stage/usr/lib/pkgconfig/clap.pc
	export PKG_CONFIG_PATH=$(CURDIR)/build/lv2-stage/usr/lib/pkgconfig; $(CC) -O2 -Wall -Wextra -Werror -std=gnu99 -D_GNU_SOURCE -o $@ $< \
	    $$($(PKG_CONFIG) --cflags --libs omx-clap-lv2)

test-lv2-link: lv2/tests/lv2_link_test build/lv2/omx-host-fixture.lv2/omx-host-fixture.so
	./lv2/tests/lv2_link_test $(abspath build/lv2/omx-host-fixture.lv2)

lv2-cost: lv2/tests/lv2_cost build/lv2/omx-lv2-fakes.lv2/omx-lv2-fakes.so
	./lv2/tests/lv2_cost build/lv2/omx-lv2-fakes.lv2
.PHONY: lv2-cost

# the fixture bundles against the LV2 specifications, with lv2_validate (the lv2 package's) when it is installed
test-lv2-validate: $(LV2_BUNDLES)
	@if command -v lv2_validate >/dev/null && command -v sord_validate >/dev/null; then \
	    for b in build/lv2/*.lv2; do lv2_validate $$b/*.ttl 2>&1 | tail -1 | tee build/lv2/validate.txt; grep -q '^Found 0 errors' build/lv2/validate.txt || exit 1; done; \
	    echo "ok   every fixture bundle validates"; \
	else echo "lv2_validate or sord_validate is not installed: the fixture bundles were not validated"; fi
.PHONY: test-lv2-validate
