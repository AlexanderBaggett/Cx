# Cx vs C head-to-head benchmarks. See README.md.
#
#   make                  build build/{c,cx,ctrl}/bench
#   make check            lint + Cx-rule scan + strict build + sanitizers + checksum parity
#   make codegen-compare  compare the machine code produced by both compilers
#   make run ROUNDS=5     run everything (tools/run.py); results in results/
#   make report           summarize the newest results file
#
# Toolchains
#   CC_C      the unchanged reference compiler: the official LLVM 23.1.2 release clang
#   CC_CX     the Cx compiler: clang built from this repository (see README.md)
#   CX_FLAGS  Cx language-mode flags for the Cx toolchain (empty until a Cx mode exists)
#   MARCH     target ISA for both toolchains (default x86-64; e.g. x86-64-v3)

LLVM_REF ?= $(HOME)/llvm-23.1.2-release
CX_BUILD ?= $(abspath ../../cx-build)
CC_C     ?= $(LLVM_REF)/bin/clang
CC_CX    ?= $(CX_BUILD)/bin/clang
CX_FLAGS ?=
MARCH    ?= x86-64
OPT      ?= -O2

# -ffp-contract=off: no fused multiply-add contraction, so floating-point
# results (and checksums) cannot depend on whether a compiler happened to
# constant-fold an expression with fused rounding.
BASEFLAGS = -std=c23 $(OPT) -march=$(MARCH) -ffp-contract=off -Iharness -Isrc \
            -Wall -Wextra -Wno-unknown-attributes -Wno-extern-initializer
LDLIBS    = -lm

# ONLY=ops|ds|alg|kern restricts the build to one category (for developing a
# category in isolation); it builds into build-$(ONLY).
ONLY     ?=
BUILD    ?= $(if $(ONLY),build-$(ONLY),build)
SRCS     := $(sort $(wildcard src/$(if $(ONLY),$(ONLY)_,)*.c)) harness/main.c harness/util.c
BASEFLAGS += $(if $(ONLY),-DBENCH_ONLY_$(ONLY),)
NAMES    := $(basename $(notdir $(SRCS)))
HDRS     := $(wildcard harness/*.cxh src/*.cxh cx/*.cxh)
OVERRIDES := $(sort $(wildcard cx/*.cx cx/*.cxh))

C_OBJS   := $(addprefix $(BUILD)/c/obj/,$(addsuffix .o,$(NAMES)))
CX_OBJS  := $(addprefix $(BUILD)/cx/obj/,$(addsuffix .o,$(NAMES)))

# A Cx-specific version cx/NAME.cx, if present, replaces the shared source for
# the Cx toolchain only. Until the Cx front end exists it is compiled as C.
cx_src = $(if $(wildcard cx/$(1).cx),cx/$(1).cx,$(filter %/$(1).c,$(SRCS)))
CX_LANG ?= -x c

.PHONY: all check lint scan strict sanitize parity codegen-compare run report clean FORCE
all: $(BUILD)/c/bench $(BUILD)/cx/bench $(BUILD)/ctrl/bench

# ---- build stamps ------------------------------------------------------------
# buildinfo.json records the compiler, its version, the flags and the Cx
# overrides. It is rewritten only when that information changes, and every
# object depends on it, so changing CC_*, CX_FLAGS, MARCH or cx/ forces a
# rebuild. tools/run.py copies it into the results metadata.
define stamp
	@printf '{"toolchain":"%s","cc":"%s","version":"%s","flags":"%s","overrides":"%s"}\n' \
	   '$(1)' '$(2)' "$$($(2) --version 2>/dev/null | head -1)" '$(strip $(3))' '$(strip $(4))' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi
endef
$(BUILD)/c/buildinfo.json: FORCE | $(BUILD)/c/obj
	$(call stamp,c,$(CC_C),$(BASEFLAGS),)
$(BUILD)/cx/buildinfo.json: FORCE | $(BUILD)/cx/obj
	$(call stamp,cx,$(CC_CX),$(BASEFLAGS) $(CX_FLAGS) $(CX_LANG),$(notdir $(OVERRIDES)))

# ---- C toolchain -------------------------------------------------------------
$(BUILD)/c/obj/%.o: src/%.c $(HDRS) $(BUILD)/c/buildinfo.json | $(BUILD)/c/obj
	$(CC_C) $(BASEFLAGS) -c $< -o $@
$(BUILD)/c/obj/%.o: harness/%.c $(HDRS) $(BUILD)/c/buildinfo.json | $(BUILD)/c/obj
	$(CC_C) $(BASEFLAGS) -c $< -o $@
$(BUILD)/c/bench: $(C_OBJS)
	$(CC_C) $(OPT) $^ -o $@ $(LDLIBS)

# ---- Cx toolchain ------------------------------------------------------------
.SECONDEXPANSION:
$(BUILD)/cx/obj/%.o: $$(call cx_src,$$*) $(HDRS) $(BUILD)/cx/buildinfo.json | $(BUILD)/cx/obj
	$(CC_CX) $(BASEFLAGS) $(CX_FLAGS) $(CX_LANG) -c $(call cx_src,$*) -o $@
$(BUILD)/cx/bench: $(CX_OBJS)
	$(CC_CX) $(OPT) $^ -o $@ $(LDLIBS)

# A/A control: a byte-identical copy of the C binary. Its spread against
# $(BUILD)/c/bench is the measurement noise floor of the machine.
$(BUILD)/ctrl/bench: $(BUILD)/c/bench | $(BUILD)/ctrl
	cp $< $@
	cp $(BUILD)/c/buildinfo.json $(BUILD)/ctrl/buildinfo.json

$(BUILD)/c/obj $(BUILD)/cx/obj $(BUILD)/ctrl $(BUILD)/strict $(BUILD)/san:
	mkdir -p $@

# ---- checks ----------------------------------------------------------------
# 1. lint:   no keywords or features that Cx removes
# 2. scan:   Cx conversion rules the warnings miss (mixed signedness after
#            promotion, implicit sign change or narrowing), from clang's AST
# 3. strict: -Werror with -Wconversion, -Wsign-conversion, -Wvla, ... (spec §7.2, §5.6)
# 4. sanitize: every benchmark runs under ASan + UBSan with the
#            unsigned-overflow and implicit-conversion checks (wraparound is a
#            violation in Cx, spec §7.1); its -O1 checksums must equal -O2's
# 5. parity: the Cx build's checksums must equal the C build's
STRICT = -Werror -Wconversion -Wsign-conversion -Wsign-compare -Wvla \
         -Wshadow -Wimplicit-fallthrough -Wstrict-prototypes -Wdouble-promotion \
         -Wcast-qual -Wpointer-arith -Wundef
SANFLAGS = -O1 -g -fno-omit-frame-pointer \
           -fsanitize=address,undefined,unsigned-integer-overflow,implicit-conversion,local-bounds \
           -fno-sanitize-recover=all

check: lint scan strict sanitize parity
lint:
	tools/check_subset.sh
scan:
	CLANG=$(CC_C) tools/cx_scan.py $(SRCS) $(filter %.cx,$(OVERRIDES))
strict: | $(BUILD)/strict
	@for f in $(SRCS) $(filter %.cx,$(OVERRIDES)); do \
	  $(CC_C) $(BASEFLAGS) $(STRICT) -x c -fsyntax-only $$f || exit 1; \
	done; echo "strict: OK ($(words $(SRCS) $(filter %.cx,$(OVERRIDES))) files)"
sanitize: $(BUILD)/c/bench | $(BUILD)/san
	$(CC_C) -std=c23 -ffp-contract=off -Iharness -Isrc -Wno-unknown-attributes -Wno-extern-initializer $(if $(ONLY),-DBENCH_ONLY_$(ONLY),) $(SANFLAGS) $(SRCS) -o $(BUILD)/san/bench $(LDLIBS)
	$(BUILD)/san/bench --all --iters 1 > $(BUILD)/san/out.jsonl
	@echo "sanitize: OK ($$(wc -l < $(BUILD)/san/out.jsonl) benchmarks ran clean)"
	$(BUILD)/c/bench --all --iters 1 > $(BUILD)/san/o2.jsonl
	tools/compare_checksums.py $(BUILD)/san/out.jsonl $(BUILD)/san/o2.jsonl
parity: $(BUILD)/c/bench $(BUILD)/cx/bench | $(BUILD)/san
	$(BUILD)/c/bench --all --iters 1 > $(BUILD)/san/c.jsonl
	$(BUILD)/cx/bench --all --iters 1 > $(BUILD)/san/cx.jsonl
	tools/compare_checksums.py $(BUILD)/san/c.jsonl $(BUILD)/san/cx.jsonl

# ---- codegen identity --------------------------------------------------------
codegen-compare: $(C_OBJS) $(CX_OBJS) $(BUILD)/c/bench $(BUILD)/cx/bench
	@tools/codegen_compare.sh $(BUILD)/c/obj $(BUILD)/cx/obj $(BUILD)/c/bench $(BUILD)/cx/bench

# ---- running ---------------------------------------------------------------
ROUNDS ?= 5
ITERS  ?= 7
run: all
	python3 tools/run.py --build $(BUILD) --rounds $(ROUNDS) --iters $(ITERS)
report:
	python3 tools/report.py

clean:
	rm -rf build build-ops build-ds build-alg build-kern
