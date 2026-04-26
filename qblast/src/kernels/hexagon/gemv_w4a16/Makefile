# Hexagon SDK driver Makefile for gemv_w4a16 DSP skel. Same boilerplate as
# qblast_hello/Makefile — see that file for what each include does.

ifndef SDK_SETUP_ENV
$(error SDK environment not set up — run 'qblast-env' first)
endif

include $(HEXAGON_SDK_ROOT)/build/make.d/utils.min

ifndef V
$(error Variant must be provided: V=hexagon_Release_toolv19_v81 (or similar))
endif

V_TARGET = $(word 1,$(subst _, ,$(V)))
ifeq ($(wildcard $(V_TARGET)_deps.min $(V_TARGET)_rules.min), )
$(error Unsupported target '$(V)' in variant '$(V)')
endif

include $(V_TARGET)_deps.min
include $(HEXAGON_SDK_ROOT)/build/make.d/$(V_TARGET)_vs.min
include $(HEXAGON_SDK_ROOT)/build/defines.min
include $(V_TARGET).min

include $(RULES_MIN)
