OUT := lmodorg

SRC := \
	src/main.c \
	src/vfs.c \
	src/mod.c

LT_PATH := lt
LT_ENV :=

# -----== COMPILER
CC := cc
CC_WARN := -Wall -Werror -Wno-strict-aliasing -Wno-error=unused-variable -Wno-unused-function -Wno-pedantic -Wno-unused-label -Wno-unused-but-set-variable
CC_FLAGS := -I$(LT_PATH)/include/ -std=gnu99 -fmax-errors=3 $(CC_WARN) -mavx2 -masm=intel

ifdef DEBUG
	CC_FLAGS += -fsanitize=undefined -fsanitize=address -fno-omit-frame-pointer -O0 -g
else
	CC_FLAGS += -O2
endif

# -----== LINKER
LNK := cc
LNK_LIBS :=
LNK_FLAGS :=

ifdef DEBUG
	LNK_LIBS += -lasan -lubsan
	LNK_FLAGS += -g -rdynamic
endif

LNK_LIBS += -lpthread -ldl -lm -lfuse3

# -----== TARGETS
ifdef DEBUG
	BIN_PATH := bin/debug
	LT_ENV += DEBUG=1
else
	BIN_PATH := bin/release
endif

OUT_PATH := $(BIN_PATH)/$(OUT)

LT_LIB := $(LT_PATH)/$(BIN_PATH)/lt.a

OBJS := $(patsubst %.c,$(BIN_PATH)/%.o,$(SRC))
DEPS := $(patsubst %.o,%.deps,$(OBJS))

all: $(OUT_PATH)

install: all
	cp $(OUT_PATH) /usr/local/bin/

run: all
	$(OUT_PATH) $(args)

clean:
	-rm -r bin

lt:
	$(LT_ENV) make -C $(LT_PATH)

$(LT_LIB): lt

$(OUT_PATH): $(OBJS) lt
	$(LNK) $(OBJS) $(LT_LIB) $(LNK_LIBS) $(LNK_FLAGS) -o $(OUT_PATH)

$(BIN_PATH)/%.o: %.c makefile
	@-mkdir -p $(BIN_PATH)/$(dir $<)
	@$(CC) $(CC_FLAGS) -MM -MT $@ -MF $(patsubst %.o,%.deps,$@) $<
	$(CC) $(CC_FLAGS) -c $< -o $@

-include $(DEPS)

.PHONY: all install run clean lt
