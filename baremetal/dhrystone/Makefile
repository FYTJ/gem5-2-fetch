COMMON_DIR = ..
TARGET = dhrystone
C_SRCS = $(shell find  -name "*.c")
CFLAGS := -I./ -I../include/ -std=gnu89
CFLAGS += -DRVTEST_EXIT_BACKEND_GEM5 -DDHRYSTONE_SILENT=1

include $(COMMON_DIR)/common.mk
