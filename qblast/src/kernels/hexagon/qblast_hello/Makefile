# Hexagon SDK driver Makefile for qblast_hello DSP skel.
# Verbatim copy of the SDK calculator_c++_apk/dsp/Makefile pattern; do not edit.
# Source qblast-env (HEXAGON_SDK_ROOT) before running.

ifndef SDK_SETUP_ENV
$(error SDK environment not set up — run 'qblast-env' (sources Hexagon SDK setup_sdk_env.source) first)
endif

include $(HEXAGON_SDK_ROOT)/build/make.d/utils.min

ifndef V
$(error Variant must be provided, pass V=<variant>; e.g. V=hexagon_Release_toolv19_v81)
endif

V_TARGET = $(word 1,$(subst _, ,$(V)))
ifeq ($(wildcard $(V_TARGET)_deps.min $(V_TARGET)_rules.min), )
$(error Unsupported target '$(V)' in variant '$(V)', check that both $(V_TARGET)_deps.min and $(V_TARGET)_rules.min exist)
endif

include $(V_TARGET)_deps.min
include $(HEXAGON_SDK_ROOT)/build/make.d/$(V_TARGET)_vs.min
include $(HEXAGON_SDK_ROOT)/build/defines.min
include $(V_TARGET).min

# always last
include $(RULES_MIN)
