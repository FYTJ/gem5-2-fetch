COMMON_DIR = ..
TARGET = coremark
C_SRCS = $(shell find  -name "*.c")
CFLAGS := -I./ -I../include/ -DITERATIONS=100
CFLAGS += -DRVTEST_EXIT_BACKEND_GEM5 -DCOREMARK_SILENT=1

include $(COMMON_DIR)/common.mk
