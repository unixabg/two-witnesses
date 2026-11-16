# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# Makefile - build the board firmware with the modules you choose (docs/MODULES.md).
#
#   make csi_defconfig       today's board: the core + CSI sensing (configs/csi_defconfig)
#   make NAME_defconfig      any configuration in configs/
#   make menuconfig          choose modules and their options (ESP-IDF's menu)
#   make savedefconfig NAME=mine   keep the current choices as configs/mine_defconfig
#   make bin                 build: out/tw_node-s3-<config>-ch11.bin
#   make release-bin         the same for csi, copied to tw_node-s3-ch11.bin (a version bump)
#   make modules             the modules found, and which this configuration has
#   make sim / sim-check     the desk simulator, and its 3-board check
#   make clean               the build folders
#
# ESP-IDF v5.5.6: IDF_EXPORT=~/esp-idf/export.sh by default.

FW      := firmware/tw_node
IDF     := tools/idf.sh
PY      ?= python3
CONFIG  := $(shell cat $(FW)/.tw_config 2>/dev/null)
OUT     := out

.PHONY: help menuconfig savedefconfig bin release-bin modules check-modules sim sim-check clean need-config

help:
	@sed -n '3,15p' Makefile | sed 's/^# \{0,1\}//'
	@echo "configurations: $$(ls configs/*_defconfig 2>/dev/null | sed 's|configs/||; s|_defconfig||' | tr '\n' ' ')"
	@echo "this one: $(or $(CONFIG),none yet (make csi_defconfig))"

%_defconfig: configs/%_defconfig
	@$(PY) tools/tw_modules.py check
	rm -f $(FW)/sdkconfig $(FW)/sdkconfig.old
	rm -rf $(FW)/build
	$(IDF) idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;../../configs/$*_defconfig" set-target esp32s3
	@echo $* > $(FW)/.tw_config
	@$(PY) tools/tw_modules.py check $(FW)/sdkconfig
	@echo "configuration: $* (make menuconfig to change it, make bin to build)"

need-config:
	@[ -n "$(CONFIG)" ] && [ -f $(FW)/sdkconfig ] || { echo "no configuration yet: make csi_defconfig (or another in configs/)"; exit 1; }

menuconfig: need-config
	@sum=$$(sha256sum $(FW)/sdkconfig); \
	$(IDF) idf.py menuconfig; \
	if [ "$$sum" != "$$(sha256sum $(FW)/sdkconfig)" ]; then \
	  case "$(CONFIG)" in *-custom) ;; *) echo "$(CONFIG)-custom" > $(FW)/.tw_config ;; esac; \
	  echo "changed: the image is named $$(cat $(FW)/.tw_config) until make savedefconfig NAME=..."; \
	fi
	@$(PY) tools/tw_modules.py check $(FW)/sdkconfig

savedefconfig: need-config
	@[ -n "$(NAME)" ] || { echo "make savedefconfig NAME=mine  (writes configs/mine_defconfig)"; exit 1; }
	@case "$(NAME)" in *[!a-z0-9_]*) echo "NAME: lower case letters, digits, _"; exit 1;; esac
	$(PY) tools/tw_modules.py savedefconfig $(FW)/sdkconfig configs/$(NAME)_defconfig
	@echo $(NAME) > $(FW)/.tw_config

bin: need-config
	@$(PY) tools/tw_modules.py check $(FW)/sdkconfig
	$(IDF) idf.py build
	@mkdir -p $(OUT)
	$(IDF) bash -c 'cd build && esptool.py --chip esp32s3 merge_bin -o "$$0" $$(cat flash_args) >/dev/null' \
	  $(CURDIR)/$(OUT)/tw_node-s3-$(CONFIG)-ch11.bin
	@sha256sum $(OUT)/tw_node-s3-$(CONFIG)-ch11.bin

release-bin: need-config
	@[ "$(CONFIG)" = csi ] || { echo "release-bin is for the csi configuration (this one: $(CONFIG))"; exit 1; }
	$(MAKE) bin
	cp $(OUT)/tw_node-s3-csi-ch11.bin tw_node-s3-ch11.bin
	@echo "tw_node-s3-ch11.bin updated: FLASH-README.md's hash, docs/CHANGES.md, tools/sums.sh"

modules:
	@$(PY) tools/tw_modules.py list $(if $(CONFIG),$(FW)/sdkconfig)

check-modules:
	@$(PY) tools/tw_modules.py check $(if $(CONFIG),$(FW)/sdkconfig)

sim:
	gcc -O2 -o firmware/sim/sim firmware/sim/sim.c $(FW)/main/tw_proto.c -lm

sim-check: sim
	cd firmware/sim && ./sim --nodes 3 --seconds 60 > /tmp/tw_sim_out.txt 2> /tmp/tw_sim_truth.json && \
	  $(PY) check_sim.py /tmp/tw_sim_out.txt /tmp/tw_sim_truth.json

clean:
	rm -rf $(FW)/build $(OUT) firmware/sim/sim
