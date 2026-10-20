# SPDX-License-Identifier: GPL-3.0-or-later
# The VST3 feasibility spike of omx-clap-core#287, NOT FOR MERGE.
#   make VST3_C_API=<a checkout of steinbergmedia/vst3_c_api at 93a116f>
#   ./vst3_probe /usr/lib64/vst3/lsp-plugins.vst3 ["Compressor Stereo"]
VST3_C_API ?= ../../../vst3_c_api
CFLAGS ?= -std=gnu11 -Wall -Wextra -O1 -g

vst3_probe: vst3_probe.c
	$(CC) $(CFLAGS) -I$(VST3_C_API) $< -o $@ -ldl -lm

clean:
	rm -f vst3_probe
.PHONY: clean
