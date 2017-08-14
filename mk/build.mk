S_FLAG := $(findstring s,$(firstword -$(MAKEFLAGS)))$(filter -s,$(MAKEFLAGS))
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
uname_O := $(shell sh -c 'uname -o 2>/dev/null || echo not')
uname_R := $(shell sh -c 'uname -r 2>/dev/null || echo not')
LIBS = -lcurses
X =

quiet_cmd_rc2c = GEN    $@

cmd_rc2c = \
    echo 'static const char *$(1) =' > $@; \
    $(SED) -f mk/rc2c.sed $< >> $@

quiet_cmd_cc = CC     $@
      cmd_cc = $(CC) $(CFLAGS) $(BASIC_CFLAGS) -c -o $@ $<

quiet_cmd_ld = LD     $@
      cmd_ld = $(LD) $(LDFLAGS) $(BASIC_LDFLAGS) -o $@ $^ $(1)

quiet_cmd_host_cc = HOSTCC $@
      cmd_host_cc = $(HOST_CC) $(HOST_CFLAGS) $(BASIC_HOST_CFLAGS) -c -o $@ $<

quiet_cmd_host_ld = HOSTLD $@
      cmd_host_ld = $(HOST_LD) $(HOST_LDFLAGS) $(BASIC_HOST_LDFLAGS) -o $@ $^ $(1)

try-run = $(shell if ($(1)) >/dev/null 2>&1; then echo "$(2)"; else echo "$(3)"; fi)
cc-option = $(call try-run, $(CC) $(1) -c -x c /dev/null -o /dev/null,$(1),$(2))

# Avoid random internationalization problems
export LC_ALL := C

ifdef S_FLAG
  cmd = $(call cmd_$(1),$(2))
else ifeq "$(V)" "1"
  cmd = @echo "$(call cmd_$(1),$(2))"; $(call cmd_$(1),$(2))
else
  cmd = @echo "   $(quiet_cmd_$(1))"; $(call cmd_$(1),$(2))
endif

ifeq "$(uname_S)" "Darwin"
  LIBS += -liconv
endif
ifeq "$(uname_O)" "Cygwin"
  LIBS += -liconv
  X = .exe
endif
ifeq "$(uname_S)" "FreeBSD"
  # libc of FreeBSD 10.0 includes iconv
  ifeq ($(shell expr "$(uname_R)" : '[0-9]\.'),2)
    LIBS += -liconv
    BASIC_CFLAGS += -I/usr/local/include
    BASIC_LDFLAGS += -L/usr/local/lib
  endif
endif
ifeq "$(uname_S)" "OpenBSD"
  LIBS += -liconv
  BASIC_CFLAGS += -I/usr/local/include
  BASIC_LDFLAGS += -L/usr/local/lib
endif
ifeq "$(uname_S)" "NetBSD"
  ifeq ($(shell expr "$(uname_R)" : '[01]\.'),2)
    LIBS += -liconv
  endif
  BASIC_CFLAGS += -I/usr/pkg/include
  BASIC_LDFLAGS += -L/usr/pkg/lib
endif

# Clang does not like container_of()
ifneq "$(CC)" "clang"
ifneq "$(uname_S)" "Darwin"
  WARNINGS += -Wcast-align
endif
endif

BASIC_CFLAGS += -std=gnu99
BASIC_CFLAGS += $(call cc-option,$(WARNINGS))
BASIC_CFLAGS += $(call cc-option,-Wno-pointer-sign) # char vs unsigned char madness

ifdef WERROR
  BASIC_CFLAGS += $(call cc-option,-Werror -Wno-error=shadow -Wno-error=unused-variable)
endif

BASIC_CFLAGS += -DDEBUG=$(DEBUG)

# Dependency generation macros copied from GIT
ifndef NO_DEPS
  CC_CAN_GENERATE_DEPS := $(call try-run,$(CC) -MMD -MP -MF /dev/null -c -x c /dev/null -o /dev/null,y,n)
  ifeq "$(CC_CAN_GENERATE_DEPS)" "n"
    NO_DEPS := y
  endif
endif

ifndef NO_DEPS
dep_files := $(foreach f,$(OBJECTS),$(dir $f).depend/$(notdir $f).d)
dep_dirs := $(addsuffix .depend,$(sort $(dir $(OBJECTS))))
missing_dep_dirs := $(filter-out $(wildcard $(dep_dirs)),$(dep_dirs))
$(OBJECTS): BASIC_CFLAGS += -MF $(dir $@).depend/$(notdir $@).d -MMD -MP
dep_files_present := $(wildcard $(dep_files))

ifneq "$(dep_files_present)" ""
include $(dep_files_present)
endif

$(dep_dirs):
	@mkdir -p $@
endif

src/vars.o: BASIC_CFLAGS += -DVERSION=\"$(VERSION)\" -DPKGDATADIR=\"$(PKGDATADIR)\"

src/main.o: src/bindings.inc
src/vars.o: .VARS
$(OBJECTS): .CFLAGS

%.o: %.c | $(missing_dep_dirs)
	$(call cmd,cc)

.CFLAGS: FORCE
	@mk/optcheck.sh "$(CC) $(CFLAGS) $(BASIC_CFLAGS)" $@

.VARS: FORCE
	@mk/optcheck.sh "VERSION=$(VERSION) PKGDATADIR=$(PKGDATADIR)" $@

src/bindings.inc: share/binding/builtin mk/rc2c.sed
	$(call cmd,rc2c,builtin_bindings)


.PHONY: FORCE
.SECONDARY: $(missing_dep_dirs)
