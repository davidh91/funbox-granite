# Project Name
TARGET = devinera
CPP_STANDARD = -std=gnu++20

USE_DAISYSP_LGPL = 1

# APP_TYPE = BOOT_SRAM

# Compiler options
OPT=-Ofast

# Build with "make LOAD_DEBUG=1" to print the CPU load over USB serial
ifdef LOAD_DEBUG
C_DEFS += -DDEVINERA_LOAD_DEBUG
endif

# Sources
CPP_SOURCES = devinera.cpp

# Library Locations
LIBDAISY_DIR = ../../libDaisy
DAISYSP_DIR = ../../DaisySP

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile


# Include funbox.h
C_INCLUDES += -I../../include
