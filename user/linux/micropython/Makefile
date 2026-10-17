# Dedicated scalar, static Linux-user port; no Unix-port dependencies.
include ../../py/mkenv.mk
QSTR_DEFS = qstrdefsport.h
MICROPY_ROM_TEXT_COMPRESSION = 0
include $(TOP)/py/py.mk
CFLAGS += -I. -I$(TOP) -I$(BUILD) -std=gnu99 -Os -Wall -Werror -DNDEBUG \
 -fno-stack-protector -fno-tree-vectorize -fno-builtin -mno-red-zone -mgeneral-regs-only \
 -ffunction-sections -fdata-sections -ffile-prefix-map=$(abspath $(TOP))=/micropython
SRC_C = main.c shared/runtime/gchelper_generic.c
OBJ = $(PY_CORE_O) $(addprefix $(BUILD)/,$(SRC_C:.c=.o))
all: $(BUILD)/micropython.elf
$(BUILD)/micropython.elf: $(OBJ)
	$(CC) -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,--gc-sections -o $@ $^
include $(TOP)/py/mkrules.mk
