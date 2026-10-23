# SPDX-License-Identifier: GPL-3.0-or-later
# compiler
CC ?= gcc

# the one version of the library, the adapter and the programs: VERSION holds it, and `make version-check` holds the
# spec's Version and the newest debian/changelog entry to it
VERSION := $(strip $(shell cat VERSION))
# the library reports it as major * 10000 + minor * 100 + patch (omx_clap_core_version())
VERSION_NUM := $(shell echo $(VERSION) | awk -F. '{ print $$1 * 10000 + $$2 * 100 + $$3 }')

# program names
PROG = omx-clap-host
SCAN_PROG = omx-clap-scan

# the hosting core, a shared library of its own: soname libomx-clap-core.so.<major>, the file <major>.<minor>.<patch>
CORE = omx-clap-core
CORE_MAJOR = 1
CORE_VERSION = $(VERSION)
CORE_SO = lib$(CORE).so
CORE_SONAME = $(CORE_SO).$(CORE_MAJOR)
CORE_FILE = $(CORE_SO).$(CORE_VERSION)
CORE_HEADERS = src/clap_host.h src/clap_host_extensions.h src/omx_msgring.h src/clap_stage.h src/hosted_stage.h src/clap_host_limits.h src/omx_clap_core_vocabulary.h src/hosted_stage_limits.h src/omx_clap_ext.h src/clap_meters.h src/omx_clap_raw.h src/omx_clap_midi.h src/clap_chain.h src/abi_guard.h
CORE_MAP = src/omx-clap-core.map
# the hosting items as a TypeScript module (data/hosting.json's render word), installed for a TypeScript build to import
CORE_TS = ts/omx-clap-core-hosting.js ts/omx-clap-core-hosting.d.ts ts/omx-clap-core-vocabulary.js ts/omx-clap-core-vocabulary.d.ts

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

# plugin-hostd-tests' protocol clients (lib/protocol.sh, lib/plugin_hostd_client.py), which every e2e test and bench
# sources: the checkout's, else the installed package's
PLUGIN_HOSTD_TESTS_LIB ?= $(firstword $(wildcard $(PLUGIN_HOSTD_DIR)/tests/lib) /usr/libexec/plugin-hostd/tests/lib)
export PLUGIN_HOSTD_TESTS_LIB

# CLAP headers: pkg-config when clap-devel is installed, CLAP_CFLAGS=-I<dir> otherwise
CLAP_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags clap 2>/dev/null)

# JACK headers and library: pkg-config when jack is installed, a JACK_DIR tree (its include and lib) otherwise
ifeq ($(shell $(PKG_CONFIG) --exists jack && echo true), true)
JACK_CFLAGS = $(shell $(PKG_CONFIG) --cflags jack)
JACK_LIBS = $(shell $(PKG_CONFIG) --libs jack)
else
JACK_DIR ?= ../jack-dev
JACK_CFLAGS = -I$(JACK_DIR)/include
JACK_LIBS = -L$(JACK_DIR)/lib -ljack -Wl,-rpath,$(abspath $(JACK_DIR)/lib)
endif

# a plugin's own window (#180, #329): Wayland needs nothing of the host's and is always built; X11 through xcb, found by
# pkg-config and optional; without it (or with X11=0) the host builds and show_external_ui tries Wayland alone
ifneq ($(X11),0)
ifeq ($(shell $(PKG_CONFIG) --exists xcb && echo true), true)
X11_CFLAGS = $(shell $(PKG_CONFIG) --cflags xcb) -DOMX_CLAP_HOST_X11
X11_LIBS = $(shell $(PKG_CONFIG) --libs xcb)
endif
endif

# plugin the test loads
CLAP_TEST_PLUGIN ?= /usr/lib/clap/omx-delay.clap

# $(1): the command that needs $(CLAP_TEST_PLUGIN). A missing plugin is a named skip, not a FAIL, on
# a desk with no omx-plugins installed; CI pins the absence red so it never passes on a skip (#200).
# The skip and $(1) share one recipe line: an early "exit 0" only ends that line's own subshell, not
# the rest of the recipe, so running $(1) has to be inside the same if/else.
define run_or_skip_clap_test_plugin
	if [ ! -e "$(CLAP_TEST_PLUGIN)" ] && [ -z "$$CI" ]; then \
		echo "skip: $(CLAP_TEST_PLUGIN) not found (install omx-plugins or set CLAP_TEST_PLUGIN)"; \
	else \
		$(1); \
	fi
endef

# omx-dsp's headers: hosted_stage.h spells no DSP word of its own (omx_block_finite, omx_denormals_off, omx_db_to_lin,
# omx_wetdry_xfade_block), it takes them from omx-dsp. pkg-config when omx-dsp-devel is installed (it names omx-contract and
# the -ffp-contract=off the kernels are built with), an omx-dsp and an omx-contract checkout otherwise
OMX_DSP_DIR ?= ../omx-dsp
OMX_CONTRACT_DIR ?= ../omx-contract
ifeq ($(shell $(PKG_CONFIG) --exists omxdsp && echo true), true)
DSP_CFLAGS = $(shell $(PKG_CONFIG) --cflags omxdsp)
else
DSP_CFLAGS = -ffp-contract=off -I$(OMX_DSP_DIR)/include -I$(OMX_CONTRACT_DIR)/include
endif

# default compiler and linker flags; everything is hidden by default, the core's export list is its version script
CFLAGS += -O3 -g -Wall -Wextra -std=gnu99 -fPIC -fvisibility=hidden -D_GNU_SOURCE -pthread -MMD -MP
CFLAGS += -Werror=implicit-function-declaration -Werror=return-type
CFLAGS += $(DSP_CFLAGS)
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
LIBS = $(JACK_LIBS) -ldl -lpthread -lm

# include paths
INCS = $(PROTOCOL_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) $(CLAP_CFLAGS) $(JACK_CFLAGS)
# plugin-hostd's include tree searched after every other, for its verb declarations alone (preset_list and preset_get,
# which plugin-hostd 0.13 and later install): a build whose pin.h comes from an older plugin-hostd still answers them
PLUGIN_HOSTD_VERBS_DIR ?=
ifneq ($(PLUGIN_HOSTD_VERBS_DIR),)
INCS += -idirafter $(PLUGIN_HOSTD_VERBS_DIR)
endif

LDFLAGS += -Wl,--no-undefined

# source and object files
SRC = src/main.c src/effects.c src/lv2_locate.c src/preset_file.c src/window.c
OBJ = $(SRC:.c=.o)
CORE_SRC = src/clap_host.c src/idle_hold.c src/clap_chain.c src/thread_pool.c
CORE_OBJ = $(CORE_SRC:.c=.o) src/abi_guard.o

# the scanner shares the plugin loading with the host through the core, and the LV2 adapter's world for --fields lv2:,
# and needs neither jack nor the protocol library
SCAN_SRC = src/scan.c src/scan_fields.c src/scan_panel.c src/lv2_locate.c
SCAN_OBJ = $(SCAN_SRC:.c=.o)

# the LV2 adapter, libomx-clap-lv2.a: lv2/core is the LV2 host with no CLAP in it, lv2/clap the shim that presents one LV2
# plugin as one CLAP plugin. A static archive, position-independent so a shared object can link it; lilv is never linked,
# only its headers are read. lv2/core's own archive is internal: its tests link it, nothing installs it.
LV2_LIB = libomx-clap-lv2.a
LV2_VERSION = $(VERSION)
LV2_CORE_LIB = build/lv2/liblv2core.a
LV2_HEADERS = lv2/clap/omx_clap_lv2.h lv2/core/omx_lv2_features.h src/abi_guard.h
LV2_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags lv2 lilv-0 2>/dev/null)
LV2_CORE_SRC = lv2/core/lv2_world.c lv2/core/lv2_features.c lv2/core/lv2_instance.c lv2/core/lv2_rtmempool.c
LV2_CORE_OBJ = $(LV2_CORE_SRC:.c=.o) lv2/core/abi_guard.o
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

# linking rule: the LV2 adapter is linked in, and lilv is not: the adapter dlopens it on the first LV2 add
$(PROG): $(OBJ) $(LV2_LIB) $(CORE_SO) $(PROTOCOL_LIB)
	$(CC) $(OBJ) $(LV2_LIB) $(CORE_LINK) $(PROTOCOL_LIBS) $(LDFLAGS) $(LIBS) $(X11_LIBS) -o $@

ifneq ($(PROTOCOL_LIB),)
$(PROTOCOL_LIB):
	$(MAKE) -C $(MOD_HOST_DIR) libmod-host-protocol.so
endif

$(SCAN_PROG): $(SCAN_OBJ) $(LV2_LIB) $(CORE_SO)
	$(CC) $(SCAN_OBJ) $(LV2_LIB) $(CORE_LINK) $(LDFLAGS) -ldl -lpthread -lm -o $@

# the preset census (tools/preset-census.c): every plugin's preset-discovery factory, preset-load and state, one TSV line
# each; built on demand, never installed
.PHONY: preset-census
preset-census: tools/preset-census

tools/preset-census: tools/preset-census.c src/clap_host.h $(CORE_SO)
	$(CC) -Isrc $(INCS) $(CFLAGS) $< $(CORE_LINK_TEST) $(LDFLAGS) -ldl -lpthread -lm -o $@

# meta-rule to generate the object files
# the objects that embed the version: a move of VERSION rebuilds them, or the library keeps answering the old number
# from an object made before the move (the staged consumer of test-core caught it once, 2026-10-20)
src/clap_host.o src/main.o src/scan.o: VERSION

%.o: %.c
	$(CC) $(INCS) $(CFLAGS) -c -o $@ $<

# the guarded plugin call (src/abi_guard.S): the core's, and the LV2 adapter's own copy under its own names, hidden
src/abi_guard.o: src/abi_guard.S
	$(CC) $(CFLAGS) -c -o $@ $<

lv2/core/abi_guard.o: src/abi_guard.S
	$(CC) $(CFLAGS) -DOMX_ABI_LV2 -c -o $@ $<

# the scanner's field dump reads an LV2 plugin through the adapter's world: lv2/core's header and the LV2 headers it names
src/scan_fields.o: src/scan_fields.c
	$(CC) -Isrc -Ilv2/core $(LV2_CFLAGS) $(INCS) $(CFLAGS) -c -o $@ $<

# the host adds an LV2 plugin through the adapter: its header, and the LV2 headers that names
src/effects.o: src/effects.c
	$(CC) -Ilv2/core -Ilv2/clap $(LV2_CFLAGS) $(INCS) $(CFLAGS) -c -o $@ $<

# the window is the one object that names xcb; the defines of X11=0 or of a build without it leave it none, so the
# object is made again whenever the choice changes
src/window.o: src/window.c build/x11-choice
	$(CC) $(X11_CFLAGS) $(INCS) $(CFLAGS) -c -o $@ $<

.PHONY: FORCE
build/x11-choice: FORCE
	@mkdir -p build
	@echo '$(X11_CFLAGS)' | cmp -s - $@ || echo '$(X11_CFLAGS)' > $@

# the host as X11=0 builds it, beside the default one, for the tests (#329): the window object with no X11 define, and
# no xcb linked
NOX11_PROG = build/nox11/$(PROG)
build/nox11/window.o: src/window.c
	@mkdir -p build/nox11
	$(CC) $(INCS) $(CFLAGS) -c -o $@ $<
$(NOX11_PROG): $(filter-out src/window.o,$(OBJ)) build/nox11/window.o $(LV2_LIB) $(CORE_SO) $(PROTOCOL_LIB)
	$(CC) $(filter-out src/window.o,$(OBJ)) build/nox11/window.o $(LV2_LIB) $(CORE_LINK) $(PROTOCOL_LIBS) $(LDFLAGS) $(LIBS) -o $@

# lv2/core names no CLAP header: its include path holds no CLAP directory
# the adapter calls its plugins through its own copy of the guard (src/abi_guard.S), named apart by OMX_ABI_LV2
lv2/core/%.o: lv2/core/%.c
	$(CC) -Isrc -Ilv2/core -DOMX_ABI_LV2 $(LV2_CFLAGS) $(CFLAGS) -c -o $@ $<

lv2/clap/%.o: lv2/clap/%.c
	$(CC) -Isrc -Ilv2/core -Ilv2/clap -DOMX_ABI_LV2 $(LV2_CFLAGS) $(CLAP_CFLAGS) $(CFLAGS) -c -o $@ $<

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
LIBEXECDIR = $(PREFIX)/libexec
UDEVDIR = $(PREFIX)/lib/udev/rules.d
# the test fixtures, in a directory of their own so no plugin scanner finds them
TESTDIR = $(LIBEXECDIR)/$(PROG)/tests
TEST_FIXTURES = tests/fake.clap tests/fake_compressor.clap tests/fault-14.clap tests/jack_meter_source tests/jack_identity \
    tests/socket_conformance.py

# the library, its headers, the pkg-config file and the export list: what a program that hosts CLAP plugins builds against
install-lib: $(CORE_SO)
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig $(DESTDIR)$(INCLUDEDIR)/omx-clap-host $(DESTDIR)$(DATADIR)/$(CORE)/ts
	install -m 755 $(CORE_FILE) $(DESTDIR)$(LIBDIR)/
	ln -sf $(CORE_FILE) $(DESTDIR)$(LIBDIR)/$(CORE_SONAME)
	ln -sf $(CORE_SONAME) $(DESTDIR)$(LIBDIR)/$(CORE_SO)
	install -m 644 $(CORE_HEADERS) $(DESTDIR)$(INCLUDEDIR)/omx-clap-host/
	sed -e 's,@PREFIX@,$(PREFIX),' -e 's,@LIBDIR@,$(LIBDIR),' -e 's,@INCLUDEDIR@,$(INCLUDEDIR),' \
	    -e 's,@DATADIR@,$(DATADIR),' -e 's,@VERSION@,$(CORE_VERSION),' omx-clap-core.pc.in > $(DESTDIR)$(LIBDIR)/pkgconfig/omx-clap-core.pc
	chmod 644 $(DESTDIR)$(LIBDIR)/pkgconfig/omx-clap-core.pc
	install -m 644 $(CORE_MAP) $(DESTDIR)$(DATADIR)/$(CORE)/
	install -m 644 $(CORE_TS) $(DESTDIR)$(DATADIR)/$(CORE)/ts/
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

# the fixtures a program that drives the worker tests itself against: a plugin that reads clap.track-info, a compressor
# that reports meters, a gain that aborts on a write of 0.9 or more (a chain's culprit), a constant jack source, and a
# feeder that plays one noise into up to six effects and records each from the same cycles
install-tests: $(TEST_FIXTURES)
	install -d $(DESTDIR)$(TESTDIR)
	install -m 755 $(TEST_FIXTURES) $(DESTDIR)$(TESTDIR)/

# the definitions this tree owns (data/: the hosting limits and refusal codes), installed as the package omx-clap-core
# into omx-contract's contract directory, <contractdir>/<schema major>/omx-clap-core/, with a MANIFEST.json naming
# this version and the tree they came from. contractdir and the install script are omx-contract-devel's (3.0.0 or
# later); a checkout of omx-contract (OMX_CONTRACT_DIR) stands in for the script when it is not installed.
CONTRACT_SCHEMA_MAJOR = 3
CONTRACTDIR = $(or $(shell $(PKG_CONFIG) --variable=contractdir omx-contract 2>/dev/null),$(DATADIR)/omx-contract/contract.d)
INSTALL_CONTRACT_DIR = $(firstword $(wildcard $(shell $(PKG_CONFIG) --variable=datadir omx-contract 2>/dev/null)/install-contract-dir.sh) $(wildcard $(OMX_CONTRACT_DIR)/tools/install-contract-dir.sh))
.PHONY: install-contract test-contract
install-contract:
	@test -n "$(INSTALL_CONTRACT_DIR)" || { echo "install-contract: no install-contract-dir.sh: install omx-contract-devel 3.0.0 or later, or set OMX_CONTRACT_DIR to a checkout" >&2; exit 1; }
	sh $(INSTALL_CONTRACT_DIR) $(CORE) $(VERSION) $(CONTRACT_SCHEMA_MAJOR) data $(DESTDIR)$(CONTRACTDIR) data

# data/ installed into a contract directory of its own, read back through omx-contract's reader beside omx-contract's
# own package, and every file this tree commits as a render of it held to `render --package omx-clap-core`, byte for
# byte (tests/contract-dir-check.sh), and every limit and refusal code of the two hosting headers held, value by value, to
# what the reader reads (tests/hosting-values-check.mjs, with its sabotage arms); node and an omx-contract checkout with its npm ci done (OMX_CONTRACT_DIR)
# omx-clap-scan --fields held to omx-contract's TERM files, and the dumps of the fake plugins and the LV2 fixture bundles
# (their binaries absent: nothing is opened) read as records; PLUGIN_HOSTD_TABLES (plugin-hostd's data/formats) holds
# every field a dump writes to a row of its table; and omx-clap-scan --json's resource hints held to omx-contract's
# RESOURCE_HINTS, words, order and units, over fake_ext's declaration (tests/resource-hints-check.py, with its sabotage arms)
PLUGIN_HOSTD_TABLES ?=
.PHONY: test-fields
test-fields: $(SCAN_PROG) tests/fake.clap tests/fake_synth.clap tests/fake_ext.clap
	python3 tests/scan-fields-check.py ./$(SCAN_PROG) $(OMX_CONTRACT_DIR)/data/formats $(abspath tests/fake.clap) $(abspath tests/fake_synth.clap) $(LV2_FIXTURE_DIR) $(PLUGIN_HOSTD_TABLES)
	python3 tests/resource-hints-check.py ./$(SCAN_PROG) $(abspath tests/fake_ext.clap) $(OMX_CONTRACT_DIR)
	python3 tests/resource-hints-check.py --self-test ./$(SCAN_PROG) $(abspath tests/fake_ext.clap) $(OMX_CONTRACT_DIR)

test-contract:
	rm -rf build/contract
	$(MAKE) --no-print-directory install-contract DESTDIR= CONTRACTDIR=$(abspath build/contract/dir) INSTALL_CONTRACT_DIR=$(OMX_CONTRACT_DIR)/tools/install-contract-dir.sh
	sh tests/contract-dir-check.sh $(OMX_CONTRACT_DIR) build/contract/dir $(VERSION)
	node $(OMX_CONTRACT_DIR)/bin/omx-contract.mjs render --target json --contract-dir build/contract/dir --out build/contract/json
	node tests/hosting-values-check.mjs build/contract/json/omx-contract.json src
	node tests/hosting-values-check.mjs --self-test build/contract/json/omx-contract.json src

install: install-lib install-lv2 install-tests install_man install-contract
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(UDEVDIR)
	install -m 755 $(PROG) $(SCAN_PROG) $(DESTDIR)$(BINDIR)
	install -m 644 packaging/udev/60-omx-clap-host.rules $(DESTDIR)$(UDEVDIR)/

install_man:
	install -d $(DESTDIR)$(MANDIR)
	for page in doc/*.1; do sed -e 's,@VERSION@,$(VERSION),' $$page > $(DESTDIR)$(MANDIR)/$$(basename $$page); chmod 644 $(DESTDIR)$(MANDIR)/$$(basename $$page); done

# the version of this tree is VERSION: the spec's Version and the newest debian/changelog entry must say the same
.PHONY: version-check print-version-num
print-version-num:
	@echo $(VERSION_NUM)

# the fixtures core_link_test runs on, in order: test-core's and the clean-root consumer steps' (ci.yml) one list
CORE_LINK_FIXTURES = tests/fake.clap tests/fake_synth.clap tests/fault-1.clap
.PHONY: print-core-link-fixtures test-ci-consumer-args
print-core-link-fixtures:
	@echo $(CORE_LINK_FIXTURES)

# the two consumer steps of ci.yml build tests/core_link_test.c by hand and run it on fixture copies: they must run it
# on CORE_LINK_FIXTURES (the same names, in order, under any directory) and compile it as this Makefile does, with
# OMX_EXPECT_VERSION_NUM from print-version-num (#275, #288: the same drift twice)
test-ci-consumer-args:
	@runs=$$(grep -E '^\s*/tmp/consumer ' .github/workflows/ci.yml | wc -l); [ "$$runs" -eq 2 ] \
	    || { echo "FAIL ci consumer: $$runs '/tmp/consumer' runs in ci.yml, two expected (RPM and DEB)"; exit 1; }
	@want="$(notdir $(CORE_LINK_FIXTURES))"; grep -E '^\s*/tmp/consumer ' .github/workflows/ci.yml | while read -r cmd args; do \
	    got=""; for a in $$args; do case $$a in '|') break ;; esac; got="$$got $${a##*/}"; done; \
	    [ "$${got# }" = "$$want" ] || { echo "FAIL ci consumer: runs on '$${got# }', test-core on '$$want'"; exit 1; }; \
	done
	@compiles=$$(grep -cE 'tests/core_link_test\.c .*\$$\(pkg-config --cflags --libs omx-clap-core\)' .github/workflows/ci.yml); [ "$$compiles" -eq 2 ] \
	    || { echo "FAIL ci consumer: $$compiles compiles of tests/core_link_test.c against the installed package, two expected"; exit 1; }
	@defines=$$(grep -cE 'DOMX_EXPECT_VERSION_NUM=\$$\(make -s print-version-num\)u -o /tmp/consumer tests/core_link_test\.c' .github/workflows/ci.yml); [ "$$defines" -eq 2 ] \
	    || { echo "FAIL ci consumer: $$defines consumer compiles carry OMX_EXPECT_VERSION_NUM from print-version-num, two expected"; exit 1; }
	@echo "ok   ci consumer: both clean-root steps build core_link_test as the Makefile does and run it on $(notdir $(CORE_LINK_FIXTURES))"

version-check:
	@spec=$$(sed -n 's/^Version:[[:space:]]*//p' packaging/omx-clap-host.spec | head -1); \
	deb=$$(sed -n '1s/^[^(]*(\([0-9][0-9.]*\).*/\1/p' debian/changelog); \
	if [ "$$spec" = "$(VERSION)" ] && [ "$$deb" = "$(VERSION)" ]; then echo "ok   VERSION $(VERSION) is the spec's Version and the newest debian/changelog entry"; \
	else echo "FAIL VERSION says $(VERSION), the spec says $$spec, debian/changelog says $$deb"; exit 1; fi

# clean rule
# No tracked file points into a private repository, issue, branch or path (tools/private-refs-check.sh, with its --self-test).
.PHONY: private-refs
private-refs:
	tools/private-refs-check.sh
	tools/private-refs-check.sh --self-test

.PHONY: docs
# the C API reference by doxygen, from the installed headers' docstrings; build-time only
docs: $(CORE_HEADERS) Doxyfile
	mkdir -p build
	doxygen Doxyfile

clean:
	@rm -rf src/*.o src/*.d tests/*.d lv2/*/*.o lv2/*/*.d $(LV2_LIB) tests/clap_state_busy_test lv2/tests/lv2_run_test lv2/tests/lv2_host_test lv2/tests/lv2_patch_test lv2/tests/lv2_features_test lv2/tests/lv2_clap_test lv2/tests/lv2_cost lv2/tests/lv2_link_test $(PROG) $(SCAN_PROG) $(CORE_SO)* build tests/clap_host_test tests/core_link_test tests/clap_scan_test tests/clap_stage_test tests/clap_core_test tests/clap_untrusted_test tests/clap_untrusted_asan tests/msgring_test tests/transport_seqlock_test tests/transport_seqlock_tsan tests/cycle_gate_tsan tests/host_transport_test tests/quiet_death_test tests/xrun_report_test tests/jack_xrun_shim.so tests/fault-*.clap tests/fake.clap tests/fake_synth.clap tests/fake_compressor.clap tests/fake_gui.clap tests/fake_panel.clap tests/crash.clap tests/jack_latency_probe tests/jack_synth_probe tests/jack_notes_out_probe tests/jack_transport_ctl tests/fake_transport.clap tests/jack_meter_source tests/jack_identity tests/jack_parity tests/clap_layout_pin tests/clap_raw_test tests/lv2_locate_test tests/param_contract_test tests/panel_abi_test tests/fake_mover.clap tests/clap_unoffered_test tests/preset_file_test tools/preset-census tests/resource_hints_test tests/thread_pool_test tests/thread_pool_tsan tests/thread_pool_bench tests/fake_pool.clap tests/fake_pool_tsan.clap

-include $(wildcard src/*.d lv2/*/*.d)

# the CLAP lifecycle against a plugin, no jack needed; the layouts the host refuses come from a fake .clap
test: test-codes test-abi-guard test-abi-sabotage tests/clap_host_test tests/fake.clap tests/fake_synth.clap test-scan test-core test-socket test-layouts-sabotage test-cycle-gate-sabotage test-xrun-report-sabotage test-clap-coverage-self test-clap-doc-check test-thread-pool test-thread-pool-sabotage test-jack-params-ready-sabotage
	./tests/clap_host_test $(CLAP_TEST_PLUGIN) $(abspath tests/fake.clap) $(abspath tests/fake_synth.clap)

# the scanner against the fake plugin, one that crashes, a broken file, a directory walk, omx-delay.clap and the fake synth
test-scan: $(SCAN_PROG) tests/clap_scan_test tests/fake.clap tests/crash.clap tests/fake_synth.clap tests/fake_panel.clap
	@$(call run_or_skip_clap_test_plugin,./tests/clap_scan_test ./$(SCAN_PROG) $(abspath tests/fake.clap) $(abspath tests/crash.clap) $(CLAP_TEST_PLUGIN) $(abspath tests/fake_synth.clap) $(abspath $(LV2_FIXTURE_DIR)) $(abspath tests/fake_panel.clap))

# the same without omx-delay.clap: only the fake plugin's checks
test-fake: test-codes tests/clap_state_busy_test tests/fake_state.clap tests/clap_host_test tests/idle_hold_test tests/fake.clap tests/fake_synth.clap tests/fake_mover.clap tests/fault-17.clap $(SCAN_PROG) tests/clap_scan_test tests/crash.clap test-core test-stage test-msgring test-transport test-raw test-quiet-death test-xrun-report test-lv2-locate test-param-contract test-preset-file test-host-transport test-chain tests/clap_ext_test tests/fake_ext.clap tests/clap_unoffered_test tests/resource_hints_test tests/fake_panel.clap test-thread-pool
	./tests/clap_ext_test $(abspath tests/fake_ext.clap)
	./tests/resource_hints_test $(abspath tests/fake_ext.clap)
	./tests/clap_unoffered_test $(abspath tests/fake.clap)
	./tests/clap_state_busy_test $(abspath tests/fake_state.clap)
	./tests/idle_hold_test $(abspath tests/fake.clap)
	./tests/clap_host_test - $(abspath tests/fake.clap) $(abspath tests/fake_synth.clap) $(abspath tests/fake_mover.clap) $(abspath tests/fault-17.clap)
	./tests/clap_scan_test ./$(SCAN_PROG) $(abspath tests/fake.clap) $(abspath tests/crash.clap) - $(abspath tests/fake_synth.clap) $(abspath $(LV2_FIXTURE_DIR)) $(abspath tests/fake_panel.clap)

# every hosting code src/ and lv2/ write is an OMX_HOSTING_ macro of src/omx_clap_core_vocabulary.h, the render of
# data/hosting/: no hand-typed "hosting." literal, no name that header lacks (tests/hosting-codes-check.sh, with its sabotage arms); and
# every verb data/verbs.json declares is an OMX_VERB_ macro of src/omx_clap_core_host_verbs.h, every reply number of
# src/host_verbs.h the declared one (tests/host-verbs-check.sh, with its sabotage arms)
.PHONY: test-codes test-verbs-source
test-codes: test-verbs-source
	bash tests/hosting-codes-check.sh
	bash tests/hosting-codes-check.sh --self-test
test-verbs-source:
	bash tests/host-verbs-check.sh
	bash tests/host-verbs-check.sh --self-test

# the stage against fake plugins at six rates, and the core over the fault fixtures and fakes: the witness, the guard
# page, the audio role on a split, the whole roster and the tempo
test-stage: tests/clap_stage_test tests/clap_core_test tests/clap_untrusted_test fixtures
	bash tests/dialect_doc_check.sh
	bash tests/dialect_doc_check.sh --self-test
	bash tests/ext_table_check.sh
	bash tests/ext_table_check.sh --self-test
	./tests/clap_stage_test
	./tests/clap_core_test $(abspath tests)
	./tests/clap_untrusted_test

# the parameters a plugin reports, with the core compiled into the test under AddressSanitizer: a read past the plugin's
# own buffers is an error here, not luck. Needs libasan; CI runs it, the package builds do not
test-untrusted-asan: tests/clap_untrusted_asan
	ASAN_OPTIONS=detect_leaks=0:check_printf=1 ./tests/clap_untrusted_asan

# omx-clap-host's transport against closed forms at every rate the contract declares: none until a tempo, the beats of
# a block from the frames played (exact over 10^6 blocks), stop, play, a tempo change, bars, the JACK transport followed
test-host-transport: tests/host_transport_test
	./tests/host_transport_test

tests/host_transport_test: tests/host_transport_test.c src/host_transport.h src/clap_stage.h tests/test_util.h
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror -o $@ $< -lm

# the worker message ring of the shared host-services layer, single- and two-threaded
test-msgring: tests/msgring_test
	./tests/msgring_test

# the published transport with an audio thread as its writer and three more reading it in the same cycle: whole records
# and no allocation (the witness); test-transport-tsan runs the same under ThreadSanitizer (needs libtsan; CI runs it,
# the package builds do not)
test-transport: tests/transport_seqlock_test
	./tests/transport_seqlock_test

# test-transport-tsan also runs the cycle gate: omx-clap-host's restart and relayout against a cycle thread that never
# stops, the core compiled into the test under ThreadSanitizer (-Wno-tsan: the core's one fence orders atomics only, which
# ThreadSanitizer sees without it)
test-transport-tsan: tests/transport_seqlock_tsan tests/cycle_gate_tsan tests/fake.clap
	TSAN_OPTIONS=halt_on_error=1 ./tests/transport_seqlock_tsan
	TSAN_OPTIONS=halt_on_error=1 ./tests/cycle_gate_tsan $(abspath tests/fake.clap)

tests/cycle_gate_tsan: tests/cycle_gate_test.c src/cycle_gate.h $(CORE_SRC) $(CORE_HEADERS)
	$(CC) -Isrc $(CLAP_CFLAGS) $(DSP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) -O1 -g -std=gnu99 -D_GNU_SOURCE -pthread -Wall -Wextra -Werror \
	    -Wno-tsan -fsanitize=thread -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u -o $@ $< $(CORE_SRC) src/abi_guard.S -ldl -lpthread -lm

# the chain's test under ThreadSanitizer, the core built into it: its cycles on a thread of their own read the plan the
# control thread publishes, and build into the other
tests/clap_chain_tsan: tests/clap_chain_test.c src/clap_chain.h tests/test_util.h $(CORE_SRC) $(CORE_HEADERS)
	$(CC) -Isrc $(CLAP_CFLAGS) $(DSP_CFLAGS) -O1 -g -std=gnu99 -D_GNU_SOURCE -pthread -Wall -Wextra -Werror \
	    -Wno-tsan -fsanitize=thread -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u -o $@ $< $(CORE_SRC) src/abi_guard.S -ldl -lpthread -lm

test-chain-tsan: tests/clap_chain_tsan tests/fake.clap tests/fake_compressor.clap
	TSAN_OPTIONS=halt_on_error=1 ./tests/clap_chain_tsan $(abspath tests/fake.clap) $(abspath tests/fake_compressor.clap)

tests/transport_seqlock_test: tests/transport_seqlock_test.c src/clap_stage.h src/hosted_stage.h
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror -DOMX_TEST_WITNESS $(WRAP_ALLOC) -pthread -o $@ $< -lm

tests/transport_seqlock_tsan: tests/transport_seqlock_test.c src/clap_stage.h src/hosted_stage.h
	$(CC) -Isrc $(CLAP_CFLAGS) -O1 -g -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=thread -pthread -o $@ $< -lm

# the sabotage of the cycle gate: a gate whose enter always answers open must let a cycle race the deactivate, which
# ThreadSanitizer reports (needs libtsan; CI runs it beside test-transport-tsan)
test-cycle-gate-sabotage: tests/cycle_gate_test.c src/cycle_gate.h $(CORE_SRC) $(CORE_HEADERS) tests/fake.clap
	mkdir -p build/cycle-gate-sabotage
	sed -e 's|    return atomic_load(&g->closed) == 0u;|    return 1;|' src/cycle_gate.h > build/cycle-gate-sabotage/cycle_gate.h
	! cmp -s src/cycle_gate.h build/cycle-gate-sabotage/cycle_gate.h
	$(CC) -Ibuild/cycle-gate-sabotage -Isrc $(CLAP_CFLAGS) $(DSP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) -O1 -g -std=gnu99 -D_GNU_SOURCE -pthread -Wall \
	    -Wextra -Werror -Wno-tsan -fsanitize=thread -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u \
	    -o build/cycle-gate-sabotage/test $< $(CORE_SRC) src/abi_guard.S -ldl -lpthread -lm
	! TSAN_OPTIONS=halt_on_error=1 ./build/cycle-gate-sabotage/test $(abspath tests/fake.clap) > build/cycle-gate-sabotage/log 2>&1
	grep -q 'ThreadSanitizer: data race' build/cycle-gate-sabotage/log

# the sabotage of test-transport: the read without its sequence re-check must let torn records through
test-transport-sabotage: tests/transport_seqlock_test.c src/clap_stage.h src/hosted_stage.h
	mkdir -p build/transport-sabotage
	sed -e '/if (atomic_load_explicit(&src->seq, memory_order_relaxed) != s1)/{N;d;}' src/clap_stage.h > build/transport-sabotage/clap_stage.h
	! grep -q 'memory_order_relaxed) != s1' build/transport-sabotage/clap_stage.h
	$(CC) -Ibuild/transport-sabotage -Isrc $(CLAP_CFLAGS) $(CFLAGS) -pthread -o build/transport-sabotage/test $< -lm
	! ./build/transport-sabotage/test

# the sabotage of the audio layouts (#92): each rule broken in a copy of the sources under build/sabotage-<arm>, the
# test that guards it built there and red on the check that names it; an arm that survives fails the target
test-layouts-sabotage:
	./tests/layout_sabotage.sh

# the chain (docs/chain.md, #311): its values over the fake plugins and stages of the test's own; and its cycle, pass_dry and
# omx_clap_chain_process, names nothing that allocates, waits, locks or prints
test-chain: tests/clap_chain_test tests/fake.clap tests/fake_compressor.clap
	mkdir -p build && sed -n '/^static void pass_dry/,/^}/p; /^int omx_clap_chain_process/,/^}/p' src/clap_chain.c > build/clap_chain_rt.c
	grep -q 'omx_clap_host_run_ports' build/clap_chain_rt.c && grep -q 'memcpy' build/clap_chain_rt.c \
	    && ! grep -n -E '\<(malloc|calloc|realloc|free|posix_memalign|usleep|nanosleep|sleep|printf|fprintf|snprintf|puts)\s*\(|pthread_|_lock\s*\(' build/clap_chain_rt.c \
	    && echo "ok   the chain's cycle (pass_dry, omx_clap_chain_process) names no allocator, sleep, lock or printf"
	./tests/clap_chain_test $(abspath tests/fake.clap) $(abspath tests/fake_compressor.clap)

# each guard of the chain broken in a copy of the tree, the test seen red on the check that names it
test-chain-sabotage:
	./tests/chain_sabotage.sh

# each guard of the resource hints (#323) broken in a copy of the tree, the test seen red on the check that names it
.PHONY: test-resource-hints-sabotage
test-resource-hints-sabotage:
	./tests/resource_hints_sabotage.sh

# the raw CLAP open a probe or an oracle runs, against the fake effect and the plugin whose init fails
test-raw: tests/clap_raw_test tests/fake.clap tests/fault-2.clap
	./tests/clap_raw_test $(abspath tests/fake.clap) $(abspath tests/fault-2.clap)

tests/clap_raw_test: tests/clap_raw_test.c src/omx_clap_raw.h tests/test_util.h
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -o $@ $< -ldl

# which bundles on an LV2 path the host opens for a URI: the manifest's spellings of it, the walk's order and stop
test-lv2-locate: tests/lv2_locate_test tests/param_contract_test
	./tests/lv2_locate_test

tests/lv2_locate_test: tests/lv2_locate_test.c src/lv2_locate.c src/lv2_locate.h tests/test_util.h
	$(CC) -Isrc $(CFLAGS) -Werror -o $@ tests/lv2_locate_test.c src/lv2_locate.c -lm

# org.openmixer.panel-description/1 as src/omx_clap_ext.h states it, held to omx-plugins' omx_panel_description.h, the
# ABI, as omx-plugins-devel installs it (pkg-config omxplugins): omx-plugins builds on this tree, so the header is read
# here, never copied
.PHONY: test-panel-abi
test-panel-abi: tests/panel_abi_test
	./tests/panel_abi_test

tests/panel_abi_test: tests/panel_abi_test.c src/omx_clap_ext.h
	@$(PKG_CONFIG) --exists omxplugins || { echo "test-panel-abi: no omx-plugins-devel (pkg-config omxplugins), whose omx_panel_description.h is the ABI" >&2; exit 1; }
	$(CC) -Isrc $(shell $(PKG_CONFIG) --cflags omxplugins) $(CLAP_CFLAGS) $(CFLAGS) -Werror -o $@ $<

# the parameter contract's SHA-256, canonical fingerprint and shortest decimals against answers computed elsewhere
test-param-contract: tests/param_contract_test
	./tests/param_contract_test

tests/param_contract_test: tests/param_contract_test.c src/param_contract.h src/omx_clap_ext.h src/abi_guard.S
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -o $@ $< src/abi_guard.S -lm

# the preset file of docs/presets.md on its own: JSON both ways, values exact, unknown members kept, base64, load keys
.PHONY: test-preset-file
test-preset-file: tests/preset_file_test
	./tests/preset_file_test

tests/preset_file_test: tests/preset_file_test.c src/preset_file.c src/preset_file.h
	$(CC) $(CFLAGS) -Werror -Isrc -o $@ tests/preset_file_test.c src/preset_file.c -lm

tests/msgring_test: tests/msgring_test.c src/omx_msgring.h src/hosted_stage_limits.h
	$(CC) $(CFLAGS) -Werror -pthread -o $@ $<

# the xrun count a worker reports (#371): what is due and when, and the count JACK's thread and the control thread share
test-xrun-report: tests/xrun_report_test
	./tests/xrun_report_test

tests/xrun_report_test: tests/xrun_report_test.c src/xrun_report.h
	$(CC) -Isrc $(CFLAGS) -Werror -pthread -o $@ $<

# the sabotage of each guard: a report with no once-a-second limit, one that writes a count already written, one that
# loses a count to a plain increment: each reds the test
test-xrun-report-sabotage: tests/xrun_report_test.c src/xrun_report.h
	mkdir -p build/xrun-report-sabotage
	for arm in rate repeat atomic; do \
	    case $$arm in \
	    rate) sed -e 's|now_ms - report->written_at_ms < XRUN_REPORT_MIN_INTERVAL_MS|now_ms == (uint64_t)-1|' src/xrun_report.h ;; \
	    repeat) sed -e 's|if (seen == report->written)|if (0)|' src/xrun_report.h ;; \
	    atomic) sed -e 's|atomic_fetch_add_explicit(&report->count, 1, memory_order_relaxed);|atomic_store_explicit(\&report->count, atomic_load_explicit(\&report->count, memory_order_relaxed) + 1, memory_order_relaxed);|' src/xrun_report.h ;; \
	    esac > build/xrun-report-sabotage/xrun_report.h; \
	    ! cmp -s src/xrun_report.h build/xrun-report-sabotage/xrun_report.h || { echo "sabotage $$arm changed nothing"; exit 1; }; \
	    $(CC) -Ibuild/xrun-report-sabotage $(CFLAGS) -Werror -pthread -o build/xrun-report-sabotage/test $<; \
	    if ./build/xrun-report-sabotage/test > build/xrun-report-sabotage/$$arm.log 2>&1; then echo "sabotage $$arm: test stayed green"; exit 1; fi; \
	    grep -q '^FAIL' build/xrun-report-sabotage/$$arm.log || { echo "sabotage $$arm: no FAIL line"; exit 1; }; \
	    echo "sabotage $$arm: red as it should"; \
	done

# a host that crashes dies of the same signal and writes no core (checked where the machine writes cores at all)
test-quiet-death: tests/quiet_death_test
	./tests/quiet_death_test

tests/quiet_death_test: tests/quiet_death_test.c src/quiet_death.h
	$(CC) -Isrc $(CFLAGS) -Werror -o $@ $<

# --wrap sees no call that link-time optimisation has already resolved inside one unit: the wrapped tests build without it
WRAP_ALLOC = -fno-lto -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

tests/clap_stage_test: tests/clap_stage_test.c src/clap_stage.h src/hosted_stage.h src/abi_guard.S
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror $(WRAP_ALLOC) -o $@ $< src/abi_guard.S -lm

tests/clap_core_test: tests/clap_core_test.c $(CORE_SO)
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror $(WRAP_ALLOC) -o $@ $< $(CORE_LINK_TEST) -lpthread -lm

# defines calloc to fail one size on demand, which the core links from it: no --wrap, no link-time optimisation, no builtin
tests/clap_untrusted_test: tests/clap_untrusted_test.c src/layout_pin.h $(CORE_SO)
	$(CC) $(CLAP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) $(CFLAGS) -Werror -fno-lto -fno-builtin-calloc -o $@ $< $(CORE_LINK_TEST) -lpthread -lm

tests/clap_untrusted_asan: tests/clap_untrusted_test.c $(CORE_SRC) $(CORE_HEADERS)
	$(CC) -Isrc $(CLAP_CFLAGS) $(DSP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) -O1 -g -std=gnu99 -D_GNU_SOURCE -pthread -Wall -Wextra -Werror -fno-omit-frame-pointer \
	    -fsanitize=address,undefined -fno-sanitize-recover=all -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u -DOMX_UNTRUSTED_ASAN -o $@ $< $(CORE_SRC) src/abi_guard.S -ldl -lpthread -lm

# the core compiled into the test, so it holds the unoffered table's flag where it lives: the library exports nothing for it
tests/clap_unoffered_test: tests/clap_unoffered_test.c $(CORE_SRC) $(CORE_HEADERS) src/abi_guard.S
	$(CC) -Isrc $(CLAP_CFLAGS) $(DSP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) -O2 -g -std=gnu99 -D_GNU_SOURCE -pthread -Wall -Wextra -Werror \
	    -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u -o $@ $< src/abi_guard.S -ldl -lpthread -lm

tests/clap_state_busy_test: tests/clap_state_busy_test.c $(CORE_SRC) $(CORE_HEADERS) src/abi_guard.S
	$(CC) -Isrc $(CLAP_CFLAGS) $(DSP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) -O2 -g -std=gnu99 -D_GNU_SOURCE -pthread -Wall -Wextra -Werror \
	    -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u -o $@ $< src/abi_guard.S -ldl -lpthread -lm

tests/clap_host_test: tests/clap_host_test.c $(CORE_SO)
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $< $(CORE_LINK_TEST) -lpthread -lm

tests/clap_chain_test: tests/clap_chain_test.c src/clap_chain.h tests/test_util.h $(CORE_SO)
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $< $(CORE_LINK_TEST) -lpthread -lm

tests/idle_hold_test: tests/idle_hold_test.c $(CORE_SO)
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $< $(CORE_LINK_TEST) -lpthread -lm

tests/clap_ext_test: tests/clap_ext_test.c $(CORE_SO)
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $< $(CORE_LINK_TEST) -ldl -lpthread -lm

# the thread pool (#395) through the core, against fake_pool's tasks: serial unless granted, spread across the helpers
# when granted, every task done before request_exec returns
tests/thread_pool_test: tests/thread_pool_test.c tests/test_util.h $(CORE_SO)
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $< $(CORE_LINK_TEST) -ldl -lpthread -lm

# and request_exec's path, the pool's hand-out and wait included (tests/rt_reach.py), names no allocator, sleep, lock or
# printf: the one syscall it makes is the futex's
test-thread-pool: tests/thread_pool_test tests/fake_pool.clap
	mkdir -p build && python3 -I tests/rt_reach.py build/thread_pool_rt.c host_request_exec src/clap_host.c src/thread_pool.c
	@for f in thread_pool_run take_tasks run_task futex_wait futex_wake; do grep -q -E "^# reached: .*\<$$f\>" build/thread_pool_rt.c || { echo "FAIL the scan of request_exec does not reach $$f"; exit 1; }; done
	! grep -n -E '\<(malloc|calloc|realloc|free|posix_memalign|usleep|nanosleep|sleep|printf|fprintf|snprintf|puts)\s*\(|pthread_mutex|pthread_cond|_lock\s*\(' build/thread_pool_rt.c \
	    || { echo "FAIL request_exec's path names an allocator, sleep, lock or printf"; false; }
	@echo "ok   request_exec and every function it reaches ($$(sed -n 's/^# reached: //p' build/thread_pool_rt.c | wc -w)) name no allocator, sleep, lock or printf"
	./tests/thread_pool_test $(abspath tests/fake_pool.clap)

# what one block of fake_pool's tasks costs, pool off against pool on (docs/thread-pool.md, Measured): a measurement
tests/thread_pool_bench: tests/thread_pool_bench.c $(CORE_SO)
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $< $(CORE_LINK_TEST) -ldl -lpthread -lm

# the same under ThreadSanitizer, the core and the fixture both built with it: the helpers' tasks, the hand-out word, the
# completion count and the pool's start and end against the audio thread and the control thread (needs libtsan)
tests/thread_pool_tsan: tests/thread_pool_test.c tests/test_util.h $(CORE_SRC) $(CORE_HEADERS)
	$(CC) -Isrc $(CLAP_CFLAGS) $(DSP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) -O1 -g -std=gnu99 -D_GNU_SOURCE -pthread -Wall -Wextra -Werror \
	    -Wno-tsan -fsanitize=thread -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u -o $@ $< $(CORE_SRC) src/abi_guard.S -ldl -lpthread -lm

tests/fake_pool_tsan.clap: tests/fake_pool.c
	$(CC) $(CLAP_CFLAGS) -O1 -g -std=gnu99 -D_GNU_SOURCE -fPIC -fvisibility=hidden -pthread -Wall -Wextra -Werror -fsanitize=thread -shared -o $@ $< -lm

test-thread-pool-tsan: tests/thread_pool_tsan tests/fake_pool_tsan.clap
	TSAN_OPTIONS=halt_on_error=1 ./tests/thread_pool_tsan $(abspath tests/fake_pool_tsan.clap)

# the sabotage of the completion wait: a pool whose request returns once the audio thread has taken its own tasks, not
# waiting for the helpers' to end, must make the test's "every task done before request_exec returned" go red
test-thread-pool-sabotage: tests/thread_pool_test.c tests/test_util.h $(CORE_SRC) $(CORE_HEADERS) tests/fake_pool.clap
	mkdir -p build/thread-pool-sabotage
	sed -e 's|^    while (atomic_load_explicit(&g_done, memory_order_acquire) < num)$$|    while (0)|' src/thread_pool.c > build/thread-pool-sabotage/thread_pool.c
	! cmp -s src/thread_pool.c build/thread-pool-sabotage/thread_pool.c
	$(CC) -Isrc $(CLAP_CFLAGS) $(DSP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) -O2 -g -std=gnu99 -D_GNU_SOURCE -pthread -Wall -Wextra -Werror \
	    -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u -o build/thread-pool-sabotage/test $< \
	    $(filter-out src/thread_pool.c,$(CORE_SRC)) build/thread-pool-sabotage/thread_pool.c src/abi_guard.S -ldl -lpthread -lm
	! ./build/thread-pool-sabotage/test $(abspath tests/fake_pool.clap) > build/thread-pool-sabotage/log 2>&1
	grep -q '^FAIL 40 requests, every task done before request_exec returned' build/thread-pool-sabotage/log || \
	    { n=$$(nproc); if [ "$$n" -lt 2 ]; then echo "skip one core: the sabotage needs a helper"; else cat build/thread-pool-sabotage/log; exit 1; fi; }
	@echo "ok   the completion wait removed, the test goes red on it"

# the plugin's resource hints (#323) through the core, against fake_ext's declaration of two
tests/resource_hints_test: tests/resource_hints_test.c tests/test_util.h $(CORE_SO)
	$(CC) $(INCS) $(CFLAGS) -Werror -o $@ $< $(CORE_LINK_TEST) -ldl -lpthread -lm

# the ABI: abidw records what the library exports and the types its public headers reach, abidiff compares a build with the
# baseline of the last release. An addition is a compatible change and a new minor; a removal or a change is a new soname major.
ABI_BASELINE = abi/$(CORE_SONAME).abi
ABI_LAYOUT = abi/$(CORE_SONAME).layout

abi-stage: $(CORE_SO)
	rm -rf build/abi
	$(MAKE) install-lib DESTDIR=$(CURDIR)/build/abi PREFIX=/usr LIBDIR=/usr/lib

# record the baseline: the release commit does this and commits abi/
abi-baseline: abi-stage
	mkdir -p abi
	abidw --headers-dir build/abi/usr/include --out-file $(ABI_BASELINE) build/abi/usr/lib/$(CORE_FILE)
	CC="$(CC)" CPPFLAGS="$(CLAP_CFLAGS) $(DSP_CFLAGS)" sh tests/abi-layout.sh build/abi/usr/include $(ABI_LAYOUT) $(VERSION) --record

# abi-check.sh's reading of abidiff, sabotaged: a fixture library whose member points to a structure, the pointee grown
# (ok) and changed five ways that must each FAIL (tests/abi-check-sabotage.sh, #323); needs gcc, abidw and abidiff
.PHONY: test-abi-sabotage
test-abi-sabotage:
	sh tests/abi-check-sabotage.sh

# libabigail's dump leaves _Atomic members out (#117): abi-layout.sh reads every member's offset, size and type with
# pahole and holds them to abi/<soname>.layout, recorded with the baseline
abi-check: abi-stage
	sh tests/abi-check.sh build/abi/usr/lib/$(CORE_FILE) build/abi/usr/include $(ABI_BASELINE)
	CC="$(CC)" CPPFLAGS="$(CLAP_CFLAGS) $(DSP_CFLAGS)" sh tests/abi-layout.sh build/abi/usr/include $(ABI_LAYOUT) $(VERSION)

# the library as a program outside this tree sees it: installed into a prefix of its own, found through its pkg-config file
# and linked by that alone, with the defaults; the export list and the libraries it names are read off the file
test-core: tests/core_link_test tests/fake.clap tests/fake_synth.clap tests/fault-1.clap $(CORE_SO)
	sh tests/exports.sh $(CORE_FILE) $(CORE_MAP) src/clap_host.h src/clap_chain.h src/abi_guard.h
	for h in $(CORE_HEADERS); do echo "#include \"$$(basename $$h)\"" | $(CC) -x c -fsyntax-only -Wall -Wextra -Werror -std=gnu99 -Isrc $(CLAP_CFLAGS) $(DSP_CFLAGS) - || exit 1; done; echo "ok   each installed header compiles on its own"
	@echo "the RT headers reach nothing that loads, allocates or waits:"; ! grep -n -E '#include <(dlfcn|pthread|stdlib|unistd|stdio)\.h>|clap_entry|dlopen|malloc|calloc' src/hosted_stage.h src/clap_stage.h src/omx_msgring.h && echo "ok   hosted_stage.h, clap_stage.h and omx_msgring.h include none of dlfcn, pthread, stdlib, unistd, stdio and name no clap_entry, dlopen or allocator"
	@echo "clap_host.h's inline RT code calls nothing that allocates, waits, locks or prints:"; mkdir -p build && sed -n '/INLINE FUNCTIONS/,$$p' src/clap_host.h > build/clap_host_inline.h && grep -q 'omx_clap_host_run(' build/clap_host_inline.h && ! grep -n -E '\<(malloc|calloc|realloc|free|usleep|nanosleep|sleep|dlopen|dlsym|printf|fprintf|snprintf|puts)\s*\(|pthread_|_lock\s*\(' build/clap_host_inline.h && echo "ok   the inline section of clap_host.h (omx_clap_host_run and the tempo word) names no allocator, sleep, lock, loader or printf"
	./tests/core_link_test $(abspath $(CORE_LINK_FIXTURES))

tests/core_link_test: tests/core_link_test.c $(CORE_SO) omx-clap-core.pc.in
	rm -rf build/stage
	$(MAKE) install-lib PREFIX=$(CURDIR)/build/stage/usr LIBDIR=$(CURDIR)/build/stage/usr/lib
	$(PKG_CONFIG) --exists clap || printf 'Name: clap\nDescription: the headers named by CLAP_CFLAGS\nVersion: 1\nCflags: $(CLAP_CFLAGS)\n' > build/stage/usr/lib/pkgconfig/clap.pc
	export PKG_CONFIG_PATH=$(CURDIR)/build/stage/usr/lib/pkgconfig$${PKG_CONFIG_PATH:+:$$PKG_CONFIG_PATH}; $(CC) -O2 -Wall -Wextra -Werror -std=gnu99 -D_GNU_SOURCE -DOMX_EXPECT_VERSION_NUM=$(VERSION_NUM)u -o $@ $< \
	    $$($(PKG_CONFIG) --cflags --libs omx-clap-core) -Wl,-rpath,$(CURDIR)/build/stage/usr/lib -lpthread -lm

# what one stage block costs, as the header is and with the four in_cycle stores relaxed (omx_clap_run's two and
# omx_clap_run_ports' two): a measurement
bench-stage: tests/stage_bench.c src/clap_stage.h src/hosted_stage.h src/omx_clap_midi.h src/abi_guard.h src/abi_guard.S
	mkdir -p build/bench-relaxed
	sed -e 's/atomic_store(&s->in_cycle, \([01]\));/atomic_store_explicit(\&s->in_cycle, \1, memory_order_relaxed);/' src/clap_stage.h > build/bench-relaxed/clap_stage.h
	test $$(grep -c 'in_cycle, [01], memory_order_relaxed' build/bench-relaxed/clap_stage.h) -eq 4
	cp src/hosted_stage.h src/clap_host_limits.h src/omx_clap_midi.h src/abi_guard.h build/bench-relaxed/
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror -DBENCH_LABEL='"seq_cst"' -o build/stage_bench_seq_cst $< src/abi_guard.S -lm
	$(CC) -Ibuild/bench-relaxed $(CLAP_CFLAGS) $(CFLAGS) -Werror -DBENCH_LABEL='"relaxed"' -o build/stage_bench_relaxed $< src/abi_guard.S -lm
	for i in 1 2 3; do ./build/stage_bench_seq_cst; ./build/stage_bench_relaxed; done

# the fault fixtures, one .clap per mode of tests/fault_clap.c: also what a consumer's tests load
FAULT_MODES = 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17
FIXTURES = $(foreach m,$(FAULT_MODES),tests/fault-$(m).clap)
fixtures: $(FIXTURES)

tests/fault-%.clap: tests/fault_clap.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -DOMX_FAULT_MODE=$* -o $@ $< -lm

tests/fake.clap: tests/fake_plugin.c src/omx_clap_ext.h
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/fake_mover.clap: tests/fake_mover.c src/omx_clap_ext.h
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/fake_panel.clap: tests/fake_panel.c src/omx_clap_ext.h
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/fake_synth.clap: tests/fake_synth.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $< -lm

tests/fake_ext.clap: tests/fake_ext.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/fake_pool.clap: tests/fake_pool.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $< -lm

tests/fake_compressor.clap: tests/fake_compressor.c src/clap_meters.h
	$(CC) -Isrc $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $< -lm

tests/fake_state.clap: tests/fake_state.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

# the window fixture opens a connection of its own to the display, as a plugin's window does (#180)
tests/fake_gui.clap: tests/fake_gui.c
	$(CC) $(CLAP_CFLAGS) $(shell $(PKG_CONFIG) --cflags xcb) $(CFLAGS) -Werror -shared -o $@ $< $(shell $(PKG_CONFIG) --libs xcb)

# the same fixture with a Wayland window too, an xdg-shell toplevel on a connection of its own (#329); xdg-shell's
# client glue is generated by wayland-scanner from wayland-protocols
XDG_SHELL_XML = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
WAYLAND_SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
build/wl/xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	@mkdir -p build/wl
	$(WAYLAND_SCANNER) client-header $< $@
build/wl/xdg-shell-protocol.c: $(XDG_SHELL_XML)
	@mkdir -p build/wl
	$(WAYLAND_SCANNER) private-code $< $@
tests/fake_gui_wl.clap: tests/fake_gui.c build/wl/xdg-shell-client-protocol.h build/wl/xdg-shell-protocol.c
	$(CC) -DFAKE_GUI_WAYLAND -Ibuild/wl $(CLAP_CFLAGS) $(shell $(PKG_CONFIG) --cflags xcb wayland-client) $(CFLAGS) -Werror -shared \
	    -o $@ $< build/wl/xdg-shell-protocol.c $(shell $(PKG_CONFIG) --libs xcb wayland-client)

tests/crash.clap: tests/crash_plugin.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/clap_scan_test: tests/clap_scan_test.c
	$(CC) $(CFLAGS) -Werror -o $@ $<

# the command socket's framing, in a PipeWire of its own: tests/socket_conformance.py against the host (#103)
test-socket: $(PROG)
	./tests/socket_e2e.sh

# the host over jack, in a PipeWire of its own: every command on the wire and the ports it makes
test-jack: $(PROG) tests/fake.clap tests/jack_latency_probe tests/jack_meter_source tests/jack_synth_probe
	CLAP_TEST_PLUGIN=$(CLAP_TEST_PLUGIN) ./tests/jack_e2e.sh

tests/jack_latency_probe: tests/jack_latency_probe.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack)

# the auxiliary ports over jack against the fake plugins alone (#92): a side chain heard, an auxiliary output read, both
# after the buffer size changes, a bypassed plugin's auxiliary output silent; CI runs it, omx-delay is not needed
test-jack-fake: $(PROG) tests/fake.clap tests/jack_meter_source tests/jack_synth_probe
	./tests/jack_fake_e2e.sh

# a plugin's own window over clap.gui and X11 (#180), in a PipeWire and an Xvfb of their own: shown, resized, hidden,
# removed open, refused by code with no display while the audio runs on; needs xcb, Xvfb, xdotool and ss
test-jack-window: $(PROG) tests/fake_gui.clap tests/fake.clap tests/jack_meter_source tests/jack_synth_probe
	./tests/jack_window_e2e.sh

# a plugin's own window over clap.gui and Wayland first (#329), in a PipeWire, a headless weston and an Xvfb of their
# own: Wayland taken when both displays are there and mapped by weston, X11 for a plugin with no Wayland window,
# no-display with neither; and the host as X11=0 builds it, links no xcb and still shows the Wayland window; needs
# wayland-client, wayland-protocols, weston, Xvfb, xdotool and ss
test-jack-window-wl: $(PROG) $(NOX11_PROG) tests/fake_gui_wl.clap tests/fake.clap tests/jack_meter_source tests/jack_synth_probe
	OMX_CLAP_HOST_NOX11=$(NOX11_PROG) ./tests/jack_window_wl_e2e.sh

# an instrument over jack in the same kind of namespace: MIDI into midi_in, the level that comes out, the silence after the note off
test-jack-synth: $(PROG) tests/fake_synth.clap tests/fake.clap tests/jack_synth_probe
	./tests/jack_synth_e2e.sh

# the transport over jack in the same kind of namespace: set_bpm, set_bpb, transport and transport_sync against a CLAP
# transport probe, then with -t the JACK transport located, started and stopped by tests/jack_transport_ctl
test-jack-transport: $(PROG) tests/fake_transport.clap tests/jack_transport_ctl
	./tests/jack_transport_e2e.sh

tests/fake_transport.clap: tests/fake_transport.c
	$(CC) $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $<

tests/jack_transport_ctl: tests/jack_transport_ctl.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack)

# a plugin's note output over jack in the same kind of namespace: one note into midi_in of a fake arpeggiator, the same in
# the MIDI dialect and the LV2 midi echo, and every message on midi_out at its frame
test-jack-notes-out: $(PROG) tests/fake_synth.clap tests/jack_notes_out_probe build/lv2/omx-midi-echo.lv2/omx-midi-echo.so
	./tests/jack_notes_out_e2e.sh

tests/jack_notes_out_probe: tests/jack_notes_out_probe.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack)

tests/jack_synth_probe: tests/jack_synth_probe.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack) -lm

# the meters over jack in the same kind of namespace: monitor_output on each derived symbol and the output_set lines the
# feedback socket carries, a constant input fed by jack_meter_source
test-jack-meters: $(PROG) tests/fake_compressor.clap tests/fake.clap tests/jack_meter_source
	./tests/jack_meters_e2e.sh

# layout pinning over jack in the same kind of namespace: pin_expect before add, a layout that matches loads, one that
# differs is destroyed before activate with nothing processed, a scheme this host does not know is refused
test-jack-pin: $(PROG) tests/fake.clap tests/clap_layout_pin
	$(HOST_PIN_DECLARED) ./tests/jack_pin_e2e.sh

# track_info, remote pages and param_info over jack in the same kind of namespace: what the plugin's clap.track-info
# reads, its two remote pages, remote_pages_changed on the feedback socket, every call on the thread that ran init
test-jack-info: $(PROG) tests/fake.clap
	$(HOST_INFO_DECLARED) ./tests/jack_info_e2e.sh

# --thread-pool over jack (#395): on makes one helper per core but one and they run the plugin's tasks, qualified (the
# default) and off make none, and removing the plugin ends them
test-jack-pool: $(PROG) tests/fake_pool.clap
	./tests/jack_pool_e2e.sh

# the xruns line over jack in the same kind of namespace (#371): the xrun callback the host registered, called by a
# preloaded test double (PipeWire's libjack calls it only when the driver reports an xrun, which a private daemon with no
# sound card never does), and the line on the feedback port: its grammar, its count, its once a second
# the abi_clobber line over jack: a plugin that returns with a callee-saved register changed, said once on the feedback
# socket, the host alive (tests/jack_abi_e2e.sh)
test-jack-abi: $(PROG) tests/fake.clap
	OMX_HOST_EVENTS_ABI_CLOBBER='$(call host_declared,OMX_HOST_EVENTS_ABI_CLOBBER)' ./tests/jack_abi_e2e.sh

test-jack-xruns: $(PROG) tests/jack_xrun_shim.so tests/fake.clap
	OMX_HOST_EVENTS_XRUNS='$(call host_declared,OMX_HOST_EVENTS_XRUNS)' ./tests/jack_xruns_e2e.sh

# the sabotage of the host's side of it: each guard of src/effects.c broken in a copy of the tree, the test red on the check
# that names it
test-jack-xruns-sabotage: $(PROG) tests/jack_xrun_shim.so
	./tests/jack_xruns_sabotage.sh

tests/jack_xrun_shim.so: tests/jack_xrun_shim.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -shared -o $@ $< $(shell $(PKG_CONFIG) --libs jack) -ldl -pthread

# a plugin that moves its own parameter over jack in the same kind of namespace: param_gesture and param_set on the
# feedback port, in order, and the values read again when there were more than the ring holds
test-jack-params: $(PROG) tests/fake_mover.clap
	./tests/jack_params_e2e.sh

# the sabotage of tests/host_ready_wait.sh (#191): a host that never logs ready!, a port a listener already held,
# and a host merely slow inside the longer bound; no jack or namespace needed, so this runs everywhere test-jack-params does not
test-jack-params-ready-sabotage: tests/host_ready_wait.sh tests/jack_params_ready_sabotage.sh
	./tests/jack_params_ready_sabotage.sh

tests/clap_layout_pin: tests/clap_layout_pin.c src/layout_pin.h src/omx_clap_raw.h
	$(CC) -Isrc $(PLUGIN_HOSTD_CFLAGS) $(CLAP_CFLAGS) $(CFLAGS) -Werror -o $@ $< -ldl

# the same behind plugin-hostd: the daemon hashes the .clap, sends pin_expect with the layout pin_set gave it, and this
# host answers the add. PLUGIN_HOSTD is the daemon, built in a plugin-hostd checkout by default.
PLUGIN_HOSTD ?= $(PLUGIN_HOSTD_DIR)/plugin-hostd
test-hostd-pin: $(PROG) tests/fake.clap tests/clap_layout_pin
	$(PHD_DECLARED) PLUGIN_HOSTD=$(PLUGIN_HOSTD) ./tests/hostd_pin_e2e.sh

# a chain behind plugin-hostd and its crash drill (#104, docs/chain.md): one node; the middle member aborts, is
# quarantined into a worker of its own, and the other two rejoin in order, their output the two one client each
test-hostd-chain: $(PROG) tests/jack_identity tests/fake_compressor.clap tests/fake_state.clap tests/fault-14.clap tests/fault-0.clap
	$(PHD_CHAIN_DECLARED) PLUGIN_HOSTD=$(PLUGIN_HOSTD) ./tests/hostd_chain_e2e.sh

# the verbs the installed declarations name for this host, against its socket and its registration table, in a PipeWire
# of its own (tests/host_verbs_e2e.sh): data/ installed into build/verbs-contract as the package installs it, unless
# OMX_CONTRACT_PATH names the contract directories to read (the packaging job's installed one)
HOST_VERBS_CONTRACT = $(abspath build/verbs-contract)
# the plugin-hostd tree whose data/ is the package read beside this one: the checkout of PLUGIN_HOSTD_DIR, or the release
# checkout CI names (the tag that ships the verbs file)
PLUGIN_HOSTD_CONTRACT_DIR ?= $(PLUGIN_HOSTD_DIR)
PLUGIN_HOSTD_VERSION = $(shell sed -n 's/^Version: *//p' $(PLUGIN_HOSTD_CONTRACT_DIR)/packaging/plugin-hostd.spec)
.PHONY: test-host-verbs test-host-verbs-sabotage host-verbs-contract
host-verbs-contract:
	rm -rf $(HOST_VERBS_CONTRACT)
	$(MAKE) --no-print-directory install-contract DESTDIR= CONTRACTDIR=$(HOST_VERBS_CONTRACT)
	@# every other package that names omx-clap-host in its verbs file (plugin-hostd's: chain, preset_list, ...) is read too (#361)
	@test -n "$(INSTALL_CONTRACT_DIR)" && test -f $(PLUGIN_HOSTD_CONTRACT_DIR)/data/verbs.json && test -n "$(PLUGIN_HOSTD_VERSION)" || { echo "host-verbs-contract: no plugin-hostd checkout at $(PLUGIN_HOSTD_CONTRACT_DIR) (PLUGIN_HOSTD_CONTRACT_DIR), whose verbs file declares chain of omx-clap-host" >&2; exit 1; }
	sh $(INSTALL_CONTRACT_DIR) plugin-hostd $(PLUGIN_HOSTD_VERSION) $(CONTRACT_SCHEMA_MAJOR) $(PLUGIN_HOSTD_CONTRACT_DIR)/data $(HOST_VERBS_CONTRACT) data
test-host-verbs: $(PROG) tests/fake.clap tests/fake_synth.clap $(if $(OMX_CONTRACT_PATH),,host-verbs-contract)
	OMX_CONTRACT_PATH=$(or $(OMX_CONTRACT_PATH),$(HOST_VERBS_CONTRACT)) $(HOST_VERBS_DECLARED) ./tests/host_verbs_e2e.sh

# the sabotage of that guard (tests/host_verbs_sabotage.sh): a format and what the host owes a verb changed in a copy of
# the declarations, a handler deleted and a verb with no declaration registered in a copy of the sources; each red
test-host-verbs-sabotage: $(PROG) tests/fake.clap tests/fake_synth.clap $(if $(OMX_CONTRACT_PATH),,host-verbs-contract)
	OMX_CONTRACT_PATH=$(or $(OMX_CONTRACT_PATH),$(HOST_VERBS_CONTRACT)) $(HOST_VERBS_DECLARED) ./tests/host_verbs_sabotage.sh

# what the tests read of the verbs data/verbs.json declares, from the header over its renders (src/host_verbs.h) and
# the layout scheme from plugin-hostd's pin.h: a test names no code or verb of its own
HASH := \#
host_declared = $(shell printf '$(HASH)include "host_verbs.h"\n$(HASH)include <plugin-hostd/pin.h>\n%s\n' $(1) | $(CC) -Isrc $(CLAP_CFLAGS) $(PLUGIN_HOSTD_CFLAGS) -E -P - | tail -n 1 | sed -e 's/^[("]//' -e 's/[)"]$$//')
HOST_PIN_DECLARED = $(foreach n,OMX_REPLY_HOST_PIN_ABSENT_RESP OMX_REPLY_HOST_PIN_LAYOUT_MISMATCH_RESP OMX_VERB_PIN_EXPECT PHD_PIN_LAYOUT_SCHEME,$(n)='$(call host_declared,$(n))')
HOST_INFO_DECLARED = $(foreach n,OMX_REPLY_HOST_NO_PARAM_CONTRACT_RESP OMX_HOST_EVENTS_REMOTE_PAGES_CHANGED,$(n)='$(call host_declared,$(n))')
HOST_VERBS_DECLARED = PHD_PIN_LAYOUT_SCHEME='$(call host_declared,PHD_PIN_LAYOUT_SCHEME)'
# what the tests behind plugin-hostd read of the daemon's protocol, from the header of the checkout they run
phd_declared = $(shell printf '$(HASH)include <plugin-hostd/protocol.h>\n$(HASH)include <plugin-hostd/pin.h>\n%s\n' $(1) | $(CC) $(PLUGIN_HOSTD_CFLAGS) -E -P - | tail -n 1 | sed -e 's/^[("]//' -e 's/[)"]$$//')
PHD_CHAIN_DECLARED = $(foreach n,PHD_READY_LINE OMX_VERB_CHAIN PHD_EVENT_CHAIN_FORMED PHD_EVENT_INSTANCE_QUARANTINED OMX_VERB_WORKER_LIST PHD_PLACE_CHAIN_PREFIX,$(n)='$(call phd_declared,$(n))')
PHD_DECLARED = $(foreach n,OMX_REPLY_HOST_PIN_ABSENT_RESP OMX_REPLY_PLUGIN_HOSTD_PIN_BINARY_MISMATCH_RESP OMX_REPLY_HOST_PIN_LAYOUT_MISMATCH_RESP OMX_VERB_PIN_EXPECT OMX_VERB_PIN_SET OMX_VERB_PIN_CLEAR PHD_PIN_LAYOUT_SCHEME PHD_READY_LINE OMX_REPLY_HOST_NO_PARAM_CONTRACT_RESP PHD_EVENT_REMOTE_PAGES_CHANGED PHD_PIN_MODE_STRICT,$(n)='$(call phd_declared,$(n))')

tests/jack_meter_source: tests/jack_meter_source.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack)

# omx-delay four ways, bit for bit, in a PipeWire of its own: its LV2 bundle through mod-host, its CLAP twin through this
# host, the LV2 bundle through this host's adapter, and through the adapter in process; omx-delay from the omx-plugins
# packages unless the script's variables say otherwise (tests/clap_lv2_identity.sh)
test-identity: $(PROG) tests/jack_identity build/bench/inproc
	@$(call run_or_skip_clap_test_plugin,./tests/clap_lv2_identity.sh)

# a chain of plugins in one JACK client against the same plugins one client each in series, bit for bit, and the latency
# the chain reports their sum (#302): the fake plugins of this tree; PLUGINS=bench/plugins.tsv for the omx-plugins set
test-chain-identity: $(PROG) tests/jack_identity tests/jack_latency_probe tests/fake.clap tests/fake_compressor.clap
	PLUGINS=$${PLUGINS:-tests/chain_fixture.tsv} ./tests/chain_identity.sh

# the chain line in one JACK client (#104, docs/chain.md): the client's process callback and every function of this
# tree it reaches (tests/rt_reach.py: chain_stage, process_gated, notes_out, transport_block, publish_gain_adjustment,
# the header functions they run, and the core's: omx_clap_chain_process, omx_clap_host_run_ports, the note and stage
# functions of clap_host.c and hosted_stage.h) names nothing that allocates, waits, locks or prints; three instances
# one node, its output the same instances one client each in series bit for bit, changed while it runs, every refusal
# changing nothing (tests/chain_host_e2e.sh)
RT_CHAIN_FILES = src/effects.c src/cycle_gate.h src/omx_msgring.h src/host_transport.h src/clap_stage.h \
    src/clap_host.c src/clap_host.h src/clap_chain.c src/hosted_stage.h
RT_CHAIN_CALLEES = chain_stage process_gated notes_out transport_block publish_gain_adjustment host_transport_read omx_cycle_gate_enter \
    omx_clap_chain_process omx_clap_host_run_ports omx_clap_host_notes_out omx_clap_host_note_in
test-chain-host: $(PROG) tests/jack_identity tests/jack_latency_probe tests/fake.clap tests/fake_compressor.clap tests/fault-14.clap
	mkdir -p build && python3 -I tests/rt_reach.py build/chain_host_rt.c process $(RT_CHAIN_FILES)
	@unreached=; for f in $(RT_CHAIN_CALLEES); do grep -q -E "^# reached: .*\<$$f\>" build/chain_host_rt.c || unreached="$$unreached $$f"; done; \
	    [ -z "$$unreached" ] || { echo "FAIL the scan of the host's chain cycle does not reach$$unreached"; false; }
	! grep -n -E '\<(malloc|calloc|realloc|free|posix_memalign|usleep|nanosleep|sleep|printf|fprintf|snprintf|puts)\s*\(|pthread_|_lock\s*\(' build/chain_host_rt.c \
	    || { echo "FAIL the host's chain cycle names an allocator, sleep, lock or printf"; false; }
	@echo "ok   the host's chain cycle and every function of this tree it reaches ($$(sed -n 's/^# reached: //p' build/chain_host_rt.c | wc -w)) name no allocator, sleep, lock or printf"
	./tests/chain_host_e2e.sh

# the chain's members walked by JACK's buffer-size callback while the control thread joins, frees, adds and lets go
# of members, the host and the core built under ThreadSanitizer: g_chain_members orders every walk against every change,
# so it reports nothing (tests/chain_host_e2e.sh with CHAIN_HOST_STRESS rounds, the quantum changed all along)
build/tsan/omx-clap-host: $(SRC) $(CORE_SRC) $(CORE_HEADERS) src/effects.h $(LV2_LIB) $(PROTOCOL_LIB)
	mkdir -p build/tsan
	$(CC) -Isrc -Ilv2/core -Ilv2/clap $(LV2_CFLAGS) $(INCS) $(X11_CFLAGS) $(DSP_CFLAGS) -O1 -g -std=gnu99 -D_GNU_SOURCE -pthread -fsanitize=thread -Wno-tsan \
	    -DOMX_CLAP_VERSION='"$(VERSION)"' -DOMX_CLAP_VERSION_NUM=$(VERSION_NUM)u -o $@ $(SRC) $(CORE_SRC) src/abi_guard.S $(LV2_LIB) \
	    $(PROTOCOL_LIBS) $(LIBS) $(X11_LIBS)

test-chain-host-tsan: build/tsan/omx-clap-host tests/jack_identity tests/jack_latency_probe tests/fake.clap tests/fake_compressor.clap tests/fault-14.clap
	./tests/chain_host_tsan.sh $(abspath build/tsan/omx-clap-host) $(abspath build/tsan)

# each guard of the host's chain broken in a copy of the tree, the test seen red on the check that names it
test-chain-host-sabotage:
	PLUGIN_HOSTD=$(PLUGIN_HOSTD) ./tests/chain_host_sabotage.sh

tests/jack_identity: tests/jack_identity.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack) -lm

# LV2 parity: every LV2 plugin that mod-host and this host both add, through both from the same cycles of one input,
# compared (tests/lv2_parity.py; the desk run is tests/lv2_parity.sh). The feeder is the graph's clock as well
tests/jack_parity: tests/jack_parity.c
	$(CC) $(shell $(PKG_CONFIG) --cflags jack libpipewire-0.3) $(CFLAGS) -Werror -o $@ $< $(shell $(PKG_CONFIG) --libs jack libpipewire-0.3) -lm

# the parity rules on fake recordings, then each rule broken on purpose and seen red (numpy)
test-parity-self:
	python3 tests/lv2_parity.py --self-test

# the CLAP coverage probe's rules: scan parsing, the host loop against a fake host, classify, each broken once and seen red
test-clap-coverage-self:
	python3 -I tests/clap_coverage.py --self-test

# docs/clap-compatibility.md against the installed CLAP headers and this tree: every id they spell a row, every cell that
# says supported naming a test that exists (tests/clap_doc_check.sh, with its sabotage arms)
.PHONY: test-clap-doc-check
test-clap-doc-check:
	bash tests/clap_doc_check.sh
	bash tests/clap_doc_check.sh --self-test

# every CLAP plugin installed on this machine added on this tree's omx-clap-host, in a private namespace with a PipeWire
# of its own: the answer per plugin and the extensions it asked for that the host did not offer. Desk only, never in CI
CLAP_COVERAGE_OUT = build/clap-coverage
CLAP_COVERAGE_DIRS = /usr/lib64/clap,/usr/lib/clap
.PHONY: test-clap-coverage-self clap-coverage
clap-coverage: $(PROG) $(SCAN_PROG)
	mkdir -p $(CLAP_COVERAGE_OUT)
	nice -n 10 python3 -I tests/clap_coverage.py scan --dirs $(CLAP_COVERAGE_DIRS) --out $(CLAP_COVERAGE_OUT)
	nice -n 10 python3 -I tests/clap_coverage.py host --sandbox --out $(CLAP_COVERAGE_OUT) --omx-clap-host ./$(PROG)
	python3 -I tests/clap_coverage.py classify --out $(CLAP_COVERAGE_OUT)

# the fixture bundle through the parity runner end to end, in a PipeWire of its own, at 48 and 96 kHz; mod-host needed
test-parity-lv2: $(PROG) tests/jack_parity $(LV2_BUNDLES)
	./tests/lv2_parity_fixture.sh

# ---- the LV2 adapter's tests ----

# the fixture bundles, built into build/lv2: their TTL beside a binary of their own, every symbol a test reads exported
LV2_FIXTURE_DIR = lv2/tests/fixtures
# -ffp-contract=off on the fixtures and the tests: an exact oracle (out == in * gain) holds only when neither side
# fuses a multiply into an add; aarch64's GCC fuses by default and the product's rounding then reads as an error
LV2_FIXTURE_CFLAGS = -O2 -g -Wall -Wextra -Werror -std=gnu99 -fPIC -shared -D_GNU_SOURCE -pthread -ffp-contract=off $(LV2_CFLAGS)
LV2_BUNDLES = build/lv2/omx-host-fixture.lv2/omx-host-fixture.so build/lv2/omx-worker-gain.lv2/omx-worker-gain.so \
              build/lv2/omx-lv2-fakes.lv2/omx-lv2-fakes.so build/lv2/omx-patch-fixture.lv2/omx-patch-fixture.so \
              build/lv2/omx-midi-echo.lv2/omx-midi-echo.so \
              build/lv2/omx-time-probe.lv2/omx-time-probe.so \
              build/lv2/omx-sidechain-fixture.lv2/omx-sidechain-fixture.so \
              build/lv2/omx-feature-probe.lv2/omx-feature-probe.so \
              build/lv2/omx-path-fixture.lv2/omx-path-fixture.so \
              build/lv2/omx-atom-fixture.lv2/omx-atom-fixture.so \
              build/lv2/omx-cv-fixture.lv2/omx-cv-fixture.so \
              build/lv2/omx-event-fixture.lv2/omx-event-fixture.so \
              build/lv2/omx-ll-midi-fixture.lv2/omx-ll-midi-fixture.so \
              build/lv2/omx-layout-fixture.lv2/omx-layout-fixture.so

# the resource hints' LV2 face (#323), outside build/lv2: the refused bundle is malformed on purpose, and lv2_validate
# knows no omxres property (omx-contract's omx-resource-hints.ttl does not validate on its own), so neither is validated
LV2_HINTS_BUNDLES = build/lv2-hints/omx-hints-fixture.lv2/omx-hints-fixture.so build/lv2-hints/omx-hints-refused.lv2/omx-hints-refused.so

build/lv2-hints/omx-hints-fixture.lv2/omx-hints-fixture.so: $(LV2_FIXTURE_DIR)/lv2_hints_fixture.c $(LV2_FIXTURE_DIR)/lv2_hints_fixture.h $(wildcard $(LV2_FIXTURE_DIR)/omx-hints-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-hints-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $<

build/lv2-hints/omx-hints-refused.lv2/omx-hints-refused.so: $(LV2_FIXTURE_DIR)/lv2_hints_fixture.c $(LV2_FIXTURE_DIR)/lv2_hints_fixture.h $(wildcard $(LV2_FIXTURE_DIR)/omx-hints-refused.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-hints-refused.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -DHINTS_REFUSED -o $@ $<

build/lv2/omx-host-fixture.lv2/omx-host-fixture.so: $(LV2_FIXTURE_DIR)/lv2_host_fixture.c $(wildcard $(LV2_FIXTURE_DIR)/omx-host-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-host-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-worker-gain.lv2/omx-worker-gain.so: $(LV2_FIXTURE_DIR)/lv2_worker_gain.c $(LV2_FIXTURE_DIR)/lv2_worker_gain.h $(wildcard $(LV2_FIXTURE_DIR)/omx-worker-gain.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-worker-gain.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -DLV2_WORKER_GAIN_BUNDLE -o $@ $< -lm

build/lv2/omx-time-probe.lv2/omx-time-probe.so: $(LV2_FIXTURE_DIR)/lv2_time_probe.c $(LV2_FIXTURE_DIR)/lv2_time_probe.h $(wildcard $(LV2_FIXTURE_DIR)/omx-time-probe.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-time-probe.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-sidechain-fixture.lv2/omx-sidechain-fixture.so: $(LV2_FIXTURE_DIR)/lv2_sidechain_fixture.c $(wildcard $(LV2_FIXTURE_DIR)/omx-sidechain-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-sidechain-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-path-fixture.lv2/omx-path-fixture.so: $(LV2_FIXTURE_DIR)/lv2_path_fixture.c $(LV2_FIXTURE_DIR)/lv2_path_probe.h $(wildcard $(LV2_FIXTURE_DIR)/omx-path-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-path-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-atom-fixture.lv2/omx-atom-fixture.so: $(LV2_FIXTURE_DIR)/lv2_atom_fixture.c $(LV2_FIXTURE_DIR)/lv2_atom_probe.h $(wildcard $(LV2_FIXTURE_DIR)/omx-atom-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-atom-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-cv-fixture.lv2/omx-cv-fixture.so: $(LV2_FIXTURE_DIR)/lv2_cv_fixture.c $(wildcard $(LV2_FIXTURE_DIR)/omx-cv-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-cv-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-event-fixture.lv2/omx-event-fixture.so: $(LV2_FIXTURE_DIR)/lv2_event_fixture.c $(LV2_FIXTURE_DIR)/lv2_event_fixture.h $(wildcard $(LV2_FIXTURE_DIR)/omx-event-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-event-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-ll-midi-fixture.lv2/omx-ll-midi-fixture.so: $(LV2_FIXTURE_DIR)/lv2_ll_midi_fixture.c $(LV2_FIXTURE_DIR)/lv2_ll_midi_fixture.h lv2/core/lv2_ll_midi.h $(wildcard $(LV2_FIXTURE_DIR)/omx-ll-midi-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-ll-midi-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-layout-fixture.lv2/omx-layout-fixture.so: $(LV2_FIXTURE_DIR)/lv2_layout_fixture.c $(wildcard $(LV2_FIXTURE_DIR)/omx-layout-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-layout-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-feature-probe.lv2/omx-feature-probe.so: $(LV2_FIXTURE_DIR)/lv2_feature_probe.c $(LV2_FIXTURE_DIR)/lv2_feature_probe.h $(wildcard $(LV2_FIXTURE_DIR)/omx-feature-probe.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-feature-probe.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -fvisibility=hidden -o $@ $< -lm

build/lv2/omx-lv2-fakes.lv2/omx-lv2-fakes.so: $(LV2_FIXTURE_DIR)/lv2_fakes.c $(LV2_FIXTURE_DIR)/lv2_fakes.h $(wildcard $(LV2_FIXTURE_DIR)/omx-lv2-fakes.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-lv2-fakes.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -fvisibility=hidden -o $@ $< -lm

build/lv2/omx-patch-fixture.lv2/omx-patch-fixture.so: $(LV2_FIXTURE_DIR)/lv2_patch_fixture.c $(wildcard $(LV2_FIXTURE_DIR)/omx-patch-fixture.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-patch-fixture.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

build/lv2/omx-midi-echo.lv2/omx-midi-echo.so: $(LV2_FIXTURE_DIR)/lv2_midi_echo.c $(wildcard $(LV2_FIXTURE_DIR)/omx-midi-echo.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-midi-echo.lv2/*.ttl $(@D)/
	$(CC) $(LV2_FIXTURE_CFLAGS) -o $@ $< -lm

LV2_TEST_CFLAGS = -Isrc -Ilv2/core -Ilv2/clap -Ilv2/tests $(LV2_CFLAGS) $(CFLAGS) -Werror -ffp-contract=off
LV2_WRAP = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

lv2/tests/lv2_run_test: lv2/tests/lv2_run_test.c $(LV2_CORE_LIB) lv2/tests/lv2_test_util.h $(LV2_FIXTURE_DIR)/lv2_fakes.h
	$(CC) $(LV2_TEST_CFLAGS) -o $@ $< $(LV2_CORE_LIB) $(LV2_WRAP) -ldl -lpthread -lm

lv2/tests/lv2_host_test: lv2/tests/lv2_host_test.c $(LV2_CORE_LIB) lv2/tests/lv2_test_util.h $(LV2_FIXTURE_DIR)/lv2_worker_gain.h $(LV2_FIXTURE_DIR)/lv2_event_fixture.h $(LV2_FIXTURE_DIR)/lv2_ll_midi_fixture.h
	$(CC) $(LV2_TEST_CFLAGS) -o $@ $< $(LV2_CORE_LIB) -Wl,--wrap=strdup,--wrap=free -ldl -lpthread -lm

lv2/tests/lv2_patch_test: lv2/tests/lv2_patch_test.c $(LV2_CORE_LIB) lv2/tests/lv2_test_util.h
	$(CC) $(LV2_TEST_CFLAGS) -o $@ $< $(LV2_CORE_LIB) -ldl -lpthread -lm

lv2/tests/lv2_features_test: lv2/tests/lv2_features_test.c $(LV2_CORE_LIB) lv2/tests/lv2_test_util.h $(LV2_FIXTURE_DIR)/lv2_feature_probe.h
	$(CC) $(LV2_TEST_CFLAGS) -o $@ $< $(LV2_CORE_LIB) -ldl -lpthread -lm

# lv2/core: the run half against the console's fakes, the bundle half and the providers against real bundles; no lilv
# is linked into either test, and lv2/core names no CLAP header
test-lv2-core: lv2/tests/lv2_run_test lv2/tests/lv2_host_test lv2/tests/lv2_patch_test lv2/tests/lv2_features_test $(LV2_BUNDLES)
	@! grep -n -E '#include [<"]clap' lv2/core/*.c lv2/core/*.h && echo "ok   lv2/core includes no CLAP header"
	@! readelf -d lv2/tests/lv2_run_test lv2/tests/lv2_host_test lv2/tests/lv2_patch_test lv2/tests/lv2_features_test | grep -q 'liblilv' && echo "ok   no test links lilv: the host dlopens it"
	./lv2/tests/lv2_run_test build/lv2/omx-lv2-fakes.lv2
	./lv2/tests/lv2_host_test build/lv2 $(LILV_LIB)
	./lv2/tests/lv2_patch_test build/lv2
	./lv2/tests/lv2_features_test build/lv2/omx-feature-probe.lv2/omx-feature-probe.so

# the MIDI output path under AddressSanitizer: lv2/core and the echo fixture built with it; a read past the adapter's own
# buffer is a report that ends the test. Needs libasan, as test-untrusted-asan does.
LV2_ASAN_DIR = build/lv2-asan
LV2_ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer -g -O1 -fPIC -D_GNU_SOURCE -std=gnu99 -Wall -Wextra -Werror -ffp-contract=off $(LV2_CFLAGS) $(DSP_CFLAGS)

$(LV2_ASAN_DIR)/omx-midi-echo.lv2/omx-midi-echo.so: $(LV2_FIXTURE_DIR)/lv2_midi_echo.c $(wildcard $(LV2_FIXTURE_DIR)/omx-midi-echo.lv2/*.ttl)
	@mkdir -p $(@D)
	cp $(LV2_FIXTURE_DIR)/omx-midi-echo.lv2/*.ttl $(@D)/
	$(CC) $(LV2_ASAN_FLAGS) -shared -o $@ $< -lm

$(LV2_ASAN_DIR)/lv2_asan_test: lv2/tests/lv2_asan_test.c $(LV2_CORE_SRC) lv2/core/lv2_core.h
	@mkdir -p $(@D)
	$(CC) $(LV2_ASAN_FLAGS) -Isrc -Ilv2/core -o $@ lv2/tests/lv2_asan_test.c $(LV2_CORE_SRC) src/abi_guard.S -ldl -lpthread -lm

# the bundle half under it too: the host test with the whole of lv2/core in the same build, run against the regular bundles
$(LV2_ASAN_DIR)/lv2_host_test: lv2/tests/lv2_host_test.c $(LV2_CORE_SRC) lv2/core/lv2_core.h lv2/tests/lv2_test_util.h $(LV2_FIXTURE_DIR)/lv2_worker_gain.h $(LV2_FIXTURE_DIR)/lv2_event_fixture.h $(LV2_FIXTURE_DIR)/lv2_ll_midi_fixture.h
	@mkdir -p $(@D)
	$(CC) $(LV2_ASAN_FLAGS) -Isrc -Ilv2/core -Ilv2/clap -Ilv2/tests -o $@ lv2/tests/lv2_host_test.c $(LV2_CORE_SRC) src/abi_guard.S -Wl,--wrap=strdup,--wrap=free -ldl -lpthread -lm

test-lv2-asan: $(LV2_ASAN_DIR)/lv2_asan_test $(LV2_ASAN_DIR)/omx-midi-echo.lv2/omx-midi-echo.so $(LV2_ASAN_DIR)/lv2_host_test $(LV2_BUNDLES)
	ASAN_OPTIONS=detect_leaks=0 ./$(LV2_ASAN_DIR)/lv2_asan_test $(LV2_ASAN_DIR)
	ASAN_OPTIONS=detect_leaks=0 ./$(LV2_ASAN_DIR)/lv2_host_test build/lv2 $(LILV_LIB)
.PHONY: test-lv2-asan

.PHONY: test-lv2 test-lv2-core test-lv2-clap test-lv2-link install-lv2
test-lv2: test-lv2-core test-lv2-clap test-lv2-link test-lv2-validate test-lv2-asan

lv2/tests/lv2_clap_test: lv2/tests/lv2_clap_test.c $(LV2_LIB) $(CORE_SO) lv2/tests/lv2_test_util.h $(LV2_FIXTURE_DIR)/lv2_fakes.h $(LV2_FIXTURE_DIR)/lv2_event_fixture.h $(LV2_FIXTURE_DIR)/lv2_ll_midi_fixture.h $(LV2_FIXTURE_DIR)/lv2_hints_fixture.h
	$(CC) $(LV2_TEST_CFLAGS) $(CLAP_CFLAGS) -o $@ $< $(LV2_LIB) $(CORE_LINK_TEST) $(LV2_WRAP) -Wl,--wrap=pthread_create -ldl -lpthread -lm

# lv2/clap: the CLAP face of the adapter, and the arms whose body is libomx-clap-core's run through it
test-lv2-clap: lv2/tests/lv2_clap_test $(LV2_BUNDLES) $(LV2_HINTS_BUNDLES)
	./lv2/tests/lv2_clap_test build/lv2

# the adapter's cost per process() call at 96 and 192 kHz, quantum 128: figures printed, nothing judged
lv2/tests/lv2_cost: lv2/tests/lv2_cost.c $(LV2_LIB) lv2/tests/lv2_test_util.h src/omx_clap_raw.h
	$(CC) $(LV2_TEST_CFLAGS) $(CLAP_CFLAGS) -o $@ $< $(LV2_LIB) -ldl -lpthread -lm

# the adapter as a program outside this tree sees it: installed into a prefix of its own, found through its pkg-config
# file and linked by that alone
lv2/tests/lv2_link_test: lv2/tests/lv2_link_test.c $(LV2_LIB) omx-clap-lv2.pc.in omx-clap-core.pc.in src/omx_clap_raw.h
	rm -rf build/lv2-stage
	$(MAKE) install-lib install-lv2 PREFIX=$(CURDIR)/build/lv2-stage/usr LIBDIR=$(CURDIR)/build/lv2-stage/usr/lib
	$(PKG_CONFIG) --exists clap || printf 'Name: clap\nDescription: the headers named by CLAP_CFLAGS\nVersion: 1\nCflags: $(CLAP_CFLAGS)\n' > build/lv2-stage/usr/lib/pkgconfig/clap.pc
	export PKG_CONFIG_PATH=$(CURDIR)/build/lv2-stage/usr/lib/pkgconfig$${PKG_CONFIG_PATH:+:$$PKG_CONFIG_PATH}; $(CC) -O2 -Wall -Wextra -Werror -std=gnu99 -D_GNU_SOURCE -o $@ $< \
	    $$($(PKG_CONFIG) --cflags --libs omx-clap-lv2)

test-lv2-link: lv2/tests/lv2_link_test build/lv2/omx-host-fixture.lv2/omx-host-fixture.so
	./lv2/tests/lv2_link_test $(abspath build/lv2/omx-host-fixture.lv2)

# an LV2 plugin through the host over jack in the same kind of namespace: the fixture bundles on LV2_PATH, lv2:<uri> and the
# bare URI, the adapter's refusals with their codes in the reply, the control ports by symbol, lv2:enabled as the bypass,
# state, presets by URI and remove; the host links the adapter and not lilv
test-jack-lv2: $(PROG) $(LV2_BUNDLES) tests/fake.clap tests/jack_meter_source tests/jack_synth_probe
	@! readelf -d $(PROG) | grep -q 'liblilv' && echo "ok   $(PROG) does not link lilv: the adapter dlopens it"
	./tests/jack_lv2_e2e.sh

# the fixture bundle three ways, bit for bit, in a PipeWire of its own: through mod-host, through this host's adapter
# (add lv2:<uri>) and through the adapter in process; IDENTITY_ARMS="lv2 inproc" where mod-host is not installed
test-identity-lv2: $(PROG) tests/jack_identity build/bench/inproc $(LV2_BUNDLES)
	IDENTITY_FIXTURE=1 ./tests/clap_lv2_identity.sh

lv2-cost: lv2/tests/lv2_cost build/lv2/omx-lv2-fakes.lv2/omx-lv2-fakes.so
	./lv2/tests/lv2_cost build/lv2/omx-lv2-fakes.lv2
.PHONY: lv2-cost

# the stage-2 oracle's kinds, each needing a fixture: exits 1 and names the missing ones. Not in test-lv2 until all five have one.
lv2-stage2-kinds:
	@bash lv2/tests/stage2_kinds.sh
	@mkdir -p build/lv2/empty-fixtures && if STAGE2_FIXTURES=$(abspath build/lv2/empty-fixtures) bash lv2/tests/stage2_kinds.sh >/dev/null 2>&1; then \
	    echo "FAIL lv2-stage2-kinds passes with no fixture at all: the check's exit status is not trusted"; exit 1; fi; \
	echo "ok   the kinds check fails on an empty fixture tree"
.PHONY: lv2-stage2-kinds

# the fixture bundles against the LV2 specifications, with lv2_validate (the lv2 package's) when it is installed
test-lv2-validate: $(LV2_BUNDLES)
	@if command -v lv2_validate >/dev/null && command -v sord_validate >/dev/null; then \
	    for b in build/lv2/*.lv2; do lv2_validate $$b/*.ttl 2>&1 | tail -1 | tee build/lv2/validate.txt; grep -q '^Found 0 errors' build/lv2/validate.txt || exit 1; done; \
	    echo "ok   every fixture bundle validates"; \
	else echo "lv2_validate or sord_validate is not installed: the fixture bundles were not validated"; fi
.PHONY: test-lv2-validate

# ---- the hosting-paths bench (docs/bench-hosting-paths-*.md) ----

# the probe (source and recorder over jack), the in-process host (a PipeWire filter over the core and the adapter) and
# the bench-only bridge that hands omx-clap-host LV2 plugins through the adapter; none of them is installed
PIPEWIRE_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
PIPEWIRE_LIBS ?= $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)
BENCH_TOOLS = build/bench/probe build/bench/inproc build/bench/omx-lv2-bridge.clap

build/bench/probe: bench/probe.c
	@mkdir -p $(@D)
	$(CC) $(JACK_CFLAGS) $(CFLAGS) -Werror -o $@ $< $(JACK_LIBS) -lm

build/bench/inproc: bench/inproc.c $(LV2_LIB) $(CORE_SO)
	@mkdir -p $(@D)
	$(CC) -Isrc -Ilv2/core -Ilv2/clap $(CLAP_CFLAGS) $(PIPEWIRE_CFLAGS) $(CFLAGS) -o $@ $< $(LV2_LIB) $(CORE_LINK_TEST) $(PIPEWIRE_LIBS) -ldl -lpthread -lm

build/bench/omx-lv2-bridge.clap: bench/lv2_bridge.c $(LV2_LIB)
	@mkdir -p $(@D)
	$(CC) -Ilv2/core -Ilv2/clap $(CLAP_CFLAGS) $(CFLAGS) -Werror -shared -o $@ $< $(LV2_LIB) -ldl -lpthread -lm

bench-tools: $(PROG) $(BENCH_TOOLS) tests/fault-0.clap tests/fault-14.clap build/lv2/omx-lv2-fakes.lv2/omx-lv2-fakes.so
.PHONY: bench bench-tools

# every path at every rate and quantum, BENCH_REPS times for BENCH_SECONDS each, then the crash drill; results under
# BENCH_OUT. Runs in private PipeWire graphs only (each run a namespace of its own); never against a live graph.
BENCH_PATHS ?= modhost clap adapter inproc-clap inproc-lv2
BENCH_RATES ?= 48000 96000
BENCH_QUANTA ?= 64 128 256 512
BENCH_REPS ?= 3
BENCH_SECONDS ?= 60
BENCH_OUT ?= build/bench/results
BENCH_IDLE_HOLD ?= default
BENCH_CRASH ?= 1
bench: bench-tools
	BENCH_PATHS="$(BENCH_PATHS)" BENCH_RATES="$(BENCH_RATES)" BENCH_QUANTA="$(BENCH_QUANTA)" BENCH_REPS=$(BENCH_REPS) \
	    BENCH_SECONDS=$(BENCH_SECONDS) BENCH_IDLE_HOLD=$(BENCH_IDLE_HOLD) BENCH_CRASH=$(BENCH_CRASH) ./bench/bench.sh $(BENCH_OUT)

# ---- the libFuzzer harnesses (fuzz/README.md) ----

# One harness per reader of bytes from disk: the preset file (fuzz_preset), the state body it carries (fuzz_state) and
# the manifest.ttl the host reads without lilv (fuzz_manifest); clang with libFuzzer, AddressSanitizer and
# UndefinedBehaviorSanitizer, into build/fuzz/. A harness links its own file, the allocation count (fuzz/alloc.c) and
# the reader's source, compiled with fuzz/alloc.h forced on it; nothing else. The ordinary build is not touched.
#   make fuzz                    build every harness
#   make fuzz-run FUZZ_TIME=60   fuzz each from its seed corpus; red on any crash, the input left in fuzz/findings/
#   make fuzz-sabotage           each mutant of fuzz/sabotage/ must make its harness abort at the check it names
#   make fuzz-records            each check's mutant: and input: record, alone, must make that check abort on that seed
#   make fuzz-coverage           the coverage witness: llvm-cov over the corpus, held to fuzz/coverage-floor.txt
#   make fuzz-coverage-self      the seed floor's own guards, sabotaged, each must turn it red; needs no build
#   make fuzz-static             each harness linked fully static with a plain driver, over its corpus
#   make fuzz-corpus            write fuzz/corpus/ again from the fixtures; fuzz-corpus-check: it is what they give
FUZZ_CC        ?= clang
FUZZ_TIME      ?= 60
FUZZ_BUILD     := build/fuzz
FUZZ_HARNESSES := preset state manifest
FUZZ_SAN       := -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ_CFLAGS    := -O1 -g -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror -fno-omit-frame-pointer
FUZZ_READER_preset   := src/preset_file.c
FUZZ_READER_state    := src/preset_file.c
FUZZ_READER_manifest := src/lv2_locate.c
FUZZ_DEPS      := fuzz/fuzz.h fuzz/alloc.h fuzz/alloc.c src/preset_file.c src/preset_file.h src/lv2_locate.c src/lv2_locate.h

# $(1): the flags of the build; the reader is instrumented for the fuzzer's feedback without linking it
define fuzz_build
	@mkdir -p $(@D)
	$(FUZZ_CC) $(1) -fsanitize=fuzzer-no-link -Isrc -include fuzz/alloc.h -c -o $@.reader.o $(FUZZ_READER_$*)
	$(FUZZ_CC) $(1) -fsanitize=fuzzer -Isrc -Ifuzz -o $@ $< fuzz/alloc.c $@.reader.o -lm
endef

$(FUZZ_BUILD)/fuzz_%: fuzz/fuzz_%.c $(FUZZ_DEPS)
	$(call fuzz_build,$(FUZZ_CFLAGS) $(FUZZ_SAN))

fuzz: $(FUZZ_HARNESSES:%=$(FUZZ_BUILD)/fuzz_%)

fuzz-run: fuzz
	FUZZ_BUILD=$(FUZZ_BUILD) FUZZ_TIME=$(FUZZ_TIME) fuzz/run.sh $(FUZZ_HARNESSES)

fuzz-sabotage:
	FUZZ_BUILD=$(FUZZ_BUILD) MAKE="$(MAKE)" fuzz/sabotage.sh

# each record built plain and native with fuzz/standalone.c, no sanitizer, as OSS Verify's coverage build is
fuzz-records:
	FUZZ_BUILD=$(FUZZ_BUILD) FUZZ_CC=$(FUZZ_CC) fuzz/records.sh

# the coverage witness: the same harnesses without sanitizers, instrumented for llvm-cov, each run once over its seed
# corpus; the llvm-cov and llvm-profdata of FUZZ_CC's own LLVM
FUZZ_COV_BUILD := $(FUZZ_BUILD)/cov
FUZZ_COV_FLAGS := -O0 -g -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror -fprofile-instr-generate -fcoverage-mapping
LLVM_COV       ?= llvm-cov
LLVM_PROFDATA  ?= llvm-profdata

$(FUZZ_COV_BUILD)/fuzz_%: fuzz/fuzz_%.c $(FUZZ_DEPS)
	$(call fuzz_build,$(FUZZ_COV_FLAGS))

fuzz-coverage: $(FUZZ_HARNESSES:%=$(FUZZ_COV_BUILD)/fuzz_%)
	@set -e; for h in $(FUZZ_HARNESSES); do \
	  rm -f $(FUZZ_COV_BUILD)/$$h.profraw; \
	  LLVM_PROFILE_FILE=$(FUZZ_COV_BUILD)/$$h.profraw $(FUZZ_COV_BUILD)/fuzz_$$h -runs=0 fuzz/corpus/$$h/ \
	      >$(FUZZ_COV_BUILD)/$$h.log 2>&1 || \
	    { cat $(FUZZ_COV_BUILD)/$$h.log; echo "fuzz-coverage: fuzz_$$h failed on its own corpus"; exit 1; }; \
	  $(LLVM_PROFDATA) merge -sparse $(FUZZ_COV_BUILD)/$$h.profraw -o $(FUZZ_COV_BUILD)/$$h.profdata; \
	done
	@LLVM_COV=$(LLVM_COV) LLVM_PROFDATA=$(LLVM_PROFDATA) fuzz/coverage.sh $(FUZZ_COV_BUILD) $(FUZZ_HARNESSES)

# the seed floor's own two guards, against itself: an empty fuzz/reach.txt and an entry naming a harness the run does
# not know must each turn it red; needs no build
fuzz-coverage-self:
	fuzz/coverage.sh --self-test

# the same harnesses driven by fuzz/standalone.c instead of libFuzzer and linked fully static, each run over its corpus:
# the build a verifier that brings its own driver takes. Needs the static libc (glibc-static)
FUZZ_STATIC_BUILD := $(FUZZ_BUILD)/static
FUZZ_STATIC_FLAGS := -O2 -g -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror

$(FUZZ_STATIC_BUILD)/fuzz_%: fuzz/fuzz_%.c fuzz/standalone.c $(FUZZ_DEPS)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_STATIC_FLAGS) -Isrc -include fuzz/alloc.h -c -o $@.reader.o $(FUZZ_READER_$*)
	$(FUZZ_CC) $(FUZZ_STATIC_FLAGS) -static -Isrc -Ifuzz -o $@ $< fuzz/alloc.c fuzz/standalone.c $@.reader.o -lm

fuzz-static: $(FUZZ_HARNESSES:%=$(FUZZ_STATIC_BUILD)/fuzz_%)
	@set -e; for h in $(FUZZ_HARNESSES); do \
	  echo "fuzz_$$h, static: $$($(FUZZ_STATIC_BUILD)/fuzz_$$h fuzz/corpus/$$h/*)"; done

# the seed corpus is written from the tree's own fixtures (fuzz/gen-corpus.c), never from a network, and committed
$(FUZZ_BUILD)/gen-corpus: fuzz/gen-corpus.c fuzz/fuzz.h src/preset_file.c src/preset_file.h src/lv2_locate.h
	@mkdir -p $(@D)
	$(FUZZ_CC) -O1 -g -std=gnu99 -Wall -Wextra -Werror -Isrc -o $@ fuzz/gen-corpus.c src/preset_file.c -lm

fuzz-corpus: $(FUZZ_BUILD)/gen-corpus
	rm -rf fuzz/corpus
	$(FUZZ_BUILD)/gen-corpus fuzz/corpus

fuzz-corpus-check: $(FUZZ_BUILD)/gen-corpus
	@set -e; tmp=$$(mktemp -d); trap 'rm -rf $$tmp' EXIT; \
	$(FUZZ_BUILD)/gen-corpus $$tmp/corpus; diff -r $$tmp/corpus fuzz/corpus; echo "the seed corpus is current"

.PHONY: fuzz fuzz-run fuzz-sabotage fuzz-records fuzz-coverage fuzz-coverage-self fuzz-static fuzz-corpus fuzz-corpus-check

# no test or bench writes its own NUL-framed send: the clients above are the one (tests/protocol_client_check.sh, which
# proves it can see a copy before it looks)
.PHONY: test-protocol-client
test-protocol-client:
	sh tests/protocol_client_check.sh

# preset_list, preset_get and preset_load over the socket (docs/presets.md): fake_synth's preset-discovery factory and
# the LV2 fixture bundle through the adapter, in a private PipeWire (tests/jack_presets_e2e.sh)
.PHONY: test-jack-presets
test-jack-presets: $(PROG) tests/fake_synth.clap tests/fake_mover.clap tests/fake_ext.clap build/lv2/omx-host-fixture.lv2/omx-host-fixture.so
	bash tests/jack_presets_e2e.sh

.PHONY: test-state-busy
test-state-busy: tests/clap_state_busy_test tests/fake_state.clap
	./tests/clap_state_busy_test $(abspath tests/fake_state.clap)

# the guarded plugin call against plugins that break the calling convention (tests/abi_guard_test.c), and its cost per
# call (tests/abi_guard_bench.c, not a gate: it prints)
tests/abi_guard_test: tests/abi_guard_test.c src/abi_guard.S src/abi_guard.h
	$(CC) $(CFLAGS) -o $@ tests/abi_guard_test.c src/abi_guard.S

tests/abi_guard_bench: tests/abi_guard_bench.c src/abi_guard.S src/abi_guard.h
	$(CC) $(CFLAGS) -o $@ tests/abi_guard_bench.c src/abi_guard.S

.PHONY: test-abi-guard bench-abi-guard
test-abi-guard: tests/abi_guard_test
	tests/abi_guard_test
	@set -e; o=$$(mktemp -d); trap 'rm -rf "$$o"' EXIT; \
	  sed 's/OMX_ABI_A(0.5)/OMX_ABI_A(5)/' tests/abi_guard_float_refused.c > $$o/int.c; \
	  $(CC) $(filter-out -MMD -MP,$(CFLAGS)) -Isrc -c -o $$o/int.o $$o/int.c \
	    || { echo "FAIL the control, an int through OMX_ABI_A, did not compile"; exit 1; }; \
	  if $(CC) $(filter-out -MMD -MP,$(CFLAGS)) -c -o $$o/double.o tests/abi_guard_float_refused.c 2> $$o/err; then \
	    echo "FAIL a double through OMX_ABI_A compiled"; exit 1; fi; \
	  grep -q omx_abi_float_refused $$o/err || { echo "FAIL the double was refused for another reason:"; cat $$o/err; exit 1; }; \
	  echo "ok   a double through OMX_ABI_A does not compile, refused by omx_abi_float_refused; an int does"

bench-abi-guard: tests/abi_guard_bench
	tests/abi_guard_bench
