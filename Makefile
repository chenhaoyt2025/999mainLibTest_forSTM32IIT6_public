# Project Name
TARGET = TDM_AK4619_DaisySP

include build_config.mk

# Sources
CPP_SOURCES = main.cpp
CPP_SOURCES += plaits_adapter.cpp
RINGS_DSP_SOURCES := $(shell find mi/ringsX_MIDI/dsp -name '*.cpp')
RINGS_DSP_SOURCES := $(filter-out \
	mi/ringsX_MIDI/dsp/string.cpp \
	mi/ringsX_MIDI/dsp/resonator.cpp, \
	$(RINGS_DSP_SOURCES))
CPP_SOURCES += $(RINGS_DSP_SOURCES)
CPP_SOURCES += rings_string_rg.cpp
CPP_SOURCES += rings_resonator_rg.cpp
CPP_SOURCES += rings_resources_rg.cpp
PLAITS_DSP_SOURCES := $(shell find mi/plaits/dsp -name '*.cpp')
PLAITS_DSP_SOURCES := $(filter-out \
	mi/plaits/dsp/physical_modelling/string.cpp \
	mi/plaits/dsp/physical_modelling/resonator.cpp, \
	$(PLAITS_DSP_SOURCES))
CPP_SOURCES += $(PLAITS_DSP_SOURCES)
CPP_SOURCES += plaits_pm_string.cpp
CPP_SOURCES += plaits_pm_resonator.cpp
CPP_SOURCES += plaits_resources_pl.cpp
CPP_SOURCES += assets_gen/vcv_knob_big.cpp
CPP_SOURCES += assets_gen/vcv_knob_small.cpp
CPP_SOURCES += mi/stmlib/dsp/units.cpp
CPP_SOURCES += mi/stmlib/dsp/atan.cpp
CPP_SOURCES += mi/stmlib/utils/random.cpp

ifeq ($(USE_PANEL_BACKGROUNDS),1)
CPP_SOURCES += assets_gen/rings_panel.cpp
CPP_SOURCES += assets_gen/plaits_panel.cpp
CPP_SOURCES += assets_gen/scope_panel.cpp
endif
C_SOURCES = \
	BSP/LTDC/lcd_rgb.c \
	BSP/LTDC/lcd_fonts.c \
	BSP/LTDC/touch_iic.c \
	BSP/LTDC/touch_800x480.c \
	SYSTEM/sys/sys.c \
	SYSTEM/delay/delay.c

C_INCLUDES += -I.
C_INCLUDES += -IBSP/LTDC
C_INCLUDES += -Imi
C_INCLUDES += -Imi/stmlib
C_INCLUDES += -Imi/ringsX_MIDI
C_INCLUDES += -Imi/plaits

C_DEFS += -DUSE_PANEL_BACKGROUNDS=$(USE_PANEL_BACKGROUNDS)

# Library Locations
LIBDAISY_DIR = ../libdaisy_IIT6/libDaisy
DAISYSP_DIR  = ../libdaisy_IIT6/DaisySP

BOARD = IIT6
IIT6_DISABLE_QSPI = 0

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
