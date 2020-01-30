VERSION = 2
PATCHLEVEL = 6
SUBLEVEL = 8
EXTRAVERSION = .1
NAME=Zonked Quokka

# *DOCUMENTATION*
# To see a list of typical targets execute "make help"
# More info can be located in ./README
# Comments in this file are targeted only to the developer, do not
# expect to learn how to build the kernel reading this file.

# Do not print "Entering directory ..."
MAKEFLAGS += --no-print-directory

# We are using a recursive build, so we need to do a little thinking
# to get the ordering right.
#
# Most importantly: sub-Makefiles should only ever modify files in
# their own directory. If in some directory we have a dependency on
# a file in another dir (which doesn't happen often, but it's of
# unavoidable when linking the built-in.o targets which finally
# turn into vmlinux), we will call a sub make in that other dir, and
# after that we are sure that everything which is in that other dir
# is now up to date.
#
# The only cases where we need to modify files which have global
# effects are thus separated out and done before the recursive
# descending is started. They are now explicitly listed as the
# prepare rule.

# To put more focus on warnings, be less verbose as default
# Use 'make V=1' to see the full commands

ifdef V
  ifeq ("$(origin V)", "command line")
    KBUILD_VERBOSE = $(V)
  endif
endif
ifndef KBUILD_VERBOSE
  KBUILD_VERBOSE = 0
endif

# Call sparse as part of compilation of C files
# Use 'make C=1' to enable sparse checking

ifdef C
  ifeq ("$(origin C)", "command line")
    KBUILD_CHECKSRC = $(C)
  endif
endif
ifndef KBUILD_CHECKSRC
  KBUILD_CHECKSRC = 0
endif

# Use make M=dir to specify directory of external module to build
# Old syntax make ... SUBDIRS=$PWD is still supported
# Setting the environment variable KBUILD_EXTMOD take precedence
ifdef SUBDIRS
  KBUILD_EXTMOD ?= $(SUBDIRS)
endif
ifdef M
  ifeq ("$(origin M)", "command line")
    KBUILD_EXTMOD := $(M)
  endif
endif


# kbuild supports saving output files in a separate directory.
# To locate output files in a separate directory two syntax'es are supported.
# In both cases the working directory must be the root of the kernel src.
# 1) O=
# Use "make O=dir/to/store/output/files/"
# 
# 2) Set KBUILD_OUTPUT
# Set the environment variable KBUILD_OUTPUT to point to the directory
# where the output files shall be placed.
# export KBUILD_OUTPUT=dir/to/store/output/files/
# make
#
# The O= assigment takes precedence over the KBUILD_OUTPUT environment variable.


# KBUILD_SRC is set on invocation of make in OBJ directory
# KBUILD_SRC is not intended to be used by the regular user (for now)
ifeq ($(KBUILD_SRC),)

# OK, Make called in directory where kernel src resides
# Do we want to locate output files in a separate directory?
ifdef O
  ifeq ("$(origin O)", "command line")
    KBUILD_OUTPUT := $(O)
  endif
endif

# That's our default target when none is given on the command line
.PHONY: _all
_all:

ifneq ($(KBUILD_OUTPUT),)
# Invoke a second make in the output directory, passing relevant variables
# check that the output directory actually exists
saved-output := $(KBUILD_OUTPUT)
KBUILD_OUTPUT := $(shell cd $(KBUILD_OUTPUT) && /bin/pwd)
$(if $(wildcard $(KBUILD_OUTPUT)),, \
     $(error output directory "$(saved-output)" does not exist))

.PHONY: $(MAKECMDGOALS)

$(filter-out _all,$(MAKECMDGOALS)) _all:
	$(if $(KBUILD_VERBOSE:1=),@)$(MAKE) -C $(KBUILD_OUTPUT)		\
	KBUILD_SRC=$(CURDIR)	     KBUILD_VERBOSE=$(KBUILD_VERBOSE)	\
	KBUILD_CHECK=$(KBUILD_CHECK) KBUILD_EXTMOD="$(KBUILD_EXTMOD)"	\
        -f $(CURDIR)/Makefile $@

# Leave processing to above invocation of make
skip-makefile := 1
endif # ifneq ($(KBUILD_OUTPUT),)
endif # ifeq ($(KBUILD_SRC),)

# We process the rest of the Makefile if this is the final invocation of make
ifeq ($(skip-makefile),)

# If building an external module we do not care about the all: rule
# but instead _all depend on modules
.PHONY: all
ifeq ($(KBUILD_EXTMOD),)
_all: all
else
_all: modules
endif

srctree		:= $(if $(KBUILD_SRC),$(KBUILD_SRC),$(CURDIR))
TOPDIR		:= $(srctree)
# FIXME - TOPDIR is obsolete, use srctree/objtree
objtree		:= $(CURDIR)
src		:= $(srctree)
obj		:= $(objtree)

VPATH		:= $(srctree)

export srctree objtree VPATH TOPDIR

KERNELRELEASE=$(VERSION).$(PATCHLEVEL).$(SUBLEVEL)$(EXTRAVERSION)

# SUBARCH tells the usermode build what the underlying arch is.  That is set
# first, and if a usermode build is happening, the "ARCH=um" on the command
# line overrides the setting of ARCH below.  If a native build is happening,
# then ARCH is assigned, getting whatever value it gets normally, and 
# SUBARCH is subsequently ignored.

SUBARCH := $(shell uname -m | sed -e s/i.86/i386/ -e s/sun4u/sparc64/ \
				  -e s/arm.*/arm/ -e s/sa110/arm/ \
				  -e s/s390x/s390/ -e s/parisc64/parisc/ )

# Cross compiling and selecting different set of gcc/bin-utils
# ---------------------------------------------------------------------------
#
# When performing cross compilation for other architectures ARCH shall be set
# to the target architecture. (See arch/* for the possibilities).
# ARCH can be set during invocation of make:
# make ARCH=ia64
# Another way is to have ARCH set in the environment.
# The default ARCH is the host where make is executed.

# CROSS_COMPILE specify the prefix used for all executables used
# during compilation. Only gcc and related bin-utils executables
# are prefixed with $(CROSS_COMPILE).
# CROSS_COMPILE can be set on the command line
# make CROSS_COMPILE=ia64-linux-
# Alternatively CROSS_COMPILE can be set in the environment.
# Default value for CROSS_COMPILE is not to prefix executables
# Note: Some architectures assign CROSS_COMPILE in their arch/*/Makefile

ARCH		?= $(SUBARCH)
CROSS_COMPILE	?=

# Architecture as present in compile.h
UTS_MACHINE := $(ARCH)

# SHELL used by kbuild
CONFIG_SHELL := $(shell if [ -x "$$BASH" ]; then echo $$BASH; \
	  else if [ -x /bin/bash ]; then echo /bin/bash; \
	  else echo sh; fi ; fi)

HOSTCC  	= gcc
HOSTCXX  	= g++
HOSTCFLAGS	= -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer
HOSTCXXFLAGS	= -O2

# 	Decide whether to build built-in, modular, or both.
#	Normally, just do built-in.

KBUILD_MODULES :=
KBUILD_BUILTIN := 1

#	If we have only "make modules", don't compile built-in objects.
#	When we're building modules with modversions, we need to consider
#	the built-in objects during the descend as well, in order to
#	make sure the checksums are uptodate before we record them.

ifeq ($(MAKECMDGOALS),modules)
  KBUILD_BUILTIN := $(if $(CONFIG_MODVERSIONS),1)
endif

#	If we have "make <whatever> modules", compile modules
#	in addition to whatever we do anyway.
#	Just "make" or "make all" shall build modules as well

ifneq ($(filter all _all modules,$(MAKECMDGOALS)),)
  KBUILD_MODULES := 1
endif

ifeq ($(MAKECMDGOALS),)
  KBUILD_MODULES := 1
endif

export KBUILD_MODULES KBUILD_BUILTIN KBUILD_VERBOSE
export KBUILD_CHECKSRC KBUILD_SRC KBUILD_EXTMOD

# Beautify output
# ---------------------------------------------------------------------------
#
# Normally, we echo the whole command before executing it. By making
# that echo $($(quiet)$(cmd)), we now have the possibility to set
# $(quiet) to choose other forms of output instead, e.g.
#
#         quiet_cmd_cc_o_c = Compiling $(RELDIR)/$@
#         cmd_cc_o_c       = $(CC) $(c_flags) -c -o $@ $<
#
# If $(quiet) is empty, the whole command will be printed.
# If it is set to "quiet_", only the short version will be printed. 
# If it is set to "silent_", nothing wil be printed at all, since
# the variable $(silent_cmd_cc_o_c) doesn't exist.
#
# A simple variant is to prefix commands with $(Q) - that's usefull
# for commands that shall be hidden in non-verbose mode.
#
#	$(Q)ln $@ :<
#
# If KBUILD_VERBOSE equals 0 then the above command will be hidden.
# If KBUILD_VERBOSE equals 1 then the above command is displayed.

ifeq ($(KBUILD_VERBOSE),1)
  quiet =
  Q =
else
  quiet=quiet_
  Q = @
endif

# If the user is running make -s (silent mode), suppress echoing of
# commands

ifneq ($(findstring s,$(MAKEFLAGS)),)
  quiet=silent_
endif

check_gcc = $(shell if $(CC) $(CFLAGS) $(1) -S -o /dev/null -xc /dev/null > /dev/null 2>&1; then echo "$(1)"; else echo "$(2)"; fi ;)

export quiet Q KBUILD_VERBOSE check_gcc

# Look for make include files relative to root of kernel src
MAKEFLAGS += --include-dir=$(srctree)

# For maximum performance (+ possibly random breakage, uncomment
# the following)

#MAKEFLAGS += -rR

# Make variables (CC, etc...)

AS		= $(CROSS_COMPILE)as
LD		= $(CROSS_COMPILE)ld
CC		= $(CROSS_COMPILE)gcc
CPP		= $(CC) -E
AR		= $(CROSS_COMPILE)ar
NM		= $(CROSS_COMPILE)nm
STRIP		= $(CROSS_COMPILE)strip
OBJCOPY		= $(CROSS_COMPILE)objcopy
OBJDUMP		= $(CROSS_COMPILE)objdump
AWK		= awk
GENKSYMS	= scripts/genksyms/genksyms
DEPMOD		= /sbin/depmod
KALLSYMS	= scripts/kallsyms
PERL		= perl
CHECK		= sparse
MODFLAGS	= -DMODULE
CFLAGS_MODULE   = $(MODFLAGS)
AFLAGS_MODULE   = $(MODFLAGS)
LDFLAGS_MODULE  = -r
CFLAGS_KERNEL	=
AFLAGS_KERNEL	=

NOSTDINC_FLAGS  = -nostdinc -iwithprefix include

CPPFLAGS        := -D__KERNEL__ -Iinclude \
		   $(if $(KBUILD_SRC),-Iinclude2 -I$(srctree)/include)

CFLAGS 		:= -Wall -Wstrict-prototypes -Wno-trigraphs \
	  	   -fno-strict-aliasing -fno-common
AFLAGS		:= -D__ASSEMBLY__

export	VERSION PATCHLEVEL SUBLEVEL EXTRAVERSION KERNELRELEASE ARCH \
	CONFIG_SHELL HOSTCC HOSTCFLAGS CROSS_COMPILE AS LD CC \
	CPP AR NM STRIP OBJCOPY OBJDUMP MAKE AWK GENKSYMS PERL UTS_MACHINE \
	HOSTCXX HOSTCXXFLAGS LDFLAGS_BLOB LDFLAGS_MODULE CHECK

export CPPFLAGS NOSTDINC_FLAGS OBJCOPYFLAGS LDFLAGS
export CFLAGS CFLAGS_KERNEL CFLAGS_MODULE 
export AFLAGS AFLAGS_KERNEL AFLAGS_MODULE

# When compiling out-of-tree modules, put MODVERDIR in the module
# tree rather than in the kernel tree. The kernel tree might
# even be read-only.
export MODVERDIR := $(if $(KBUILD_EXTMOD),$(firstword $(KBUILD_EXTMOD))/).tmp_versions

# The temporary file to save gcc -MD generated dependencies must not
# contain a comma
comma := ,
depfile = $(subst $(comma),_,$(@D)/.$(@F).d)

# Files to ignore in find ... statements

RCS_FIND_IGNORE := \( -name SCCS -o -name BitKeeper -o -name .svn -o -name CVS -o -name .pc \) -prune -o
RCS_TAR_IGNORE := --exclude SCCS --exclude BitKeeper --exclude .svn --exclude CVS --exclude .pc

# ===========================================================================
# Rules shared between *config targets and build targets

# Basic helpers built in scripts/
.PHONY: scripts_basic
scripts_basic:
	$(Q)$(MAKE) $(build)=scripts/basic

# To make sure we do not include .config for any of the *config targets
# catch them early, and hand them over to scripts/kconfig/Makefile
# It is allowed to specify more targets when calling make, including
# mixing *config targets and build targets.
# For example 'make oldconfig all'. 
# Detect when mixed targets is specified, and make a second invocation
# of make so .config is not included in this case either (for *config).

no-dot-config-targets := clean mrproper distclean \
			 cscope TAGS tags help %docs check%

config-targets := 0
mixed-targets  := 0
dot-config     := 1

ifneq ($(filter $(no-dot-config-targets), $(MAKECMDGOALS)),)
	ifeq ($(filter-out $(no-dot-config-targets), $(MAKECMDGOALS)),)
		dot-config := 0
	endif
endif

ifeq ($(KBUILD_EXTMOD),)
        ifneq ($(filter config %config,$(MAKECMDGOALS)),)
                config-targets := 1
                ifneq ($(filter-out config %config,$(MAKECMDGOALS)),)
                        mixed-targets := 1
                endif
        endif
endif

ifeq ($(mixed-targets),1)
# ===========================================================================
# We're called with mixed targets (*config and build targets).
# Handle them one by one.

%:: FORCE
	$(Q)$(MAKE) -C $(srctree) KBUILD_SRC= $@

else
ifeq ($(config-targets),1)
# ===========================================================================
# *config targets only - make sure prerequisites are updated, and descend
# in scripts/kconfig to make the *config target

config: scripts_basic FORCE
	$(Q)$(MAKE) $(build)=scripts/kconfig $@
%config: scripts_basic FORCE
	$(Q)$(MAKE) $(build)=scripts/kconfig $@

else
# ===========================================================================
# Build targets only - this includes vmlinux, arch specific targets, clean
# targets and others. In general all targets except *config targets.

ifeq ($(KBUILD_EXTMOD),)
# Additional helpers built in scripts/
# Carefully list dependencies so we do not try to build scripts twice
# in parrallel
.PHONY: scripts
scripts: scripts_basic include/config/MARKER
	$(Q)$(MAKE) $(build)=$(@)

scripts_basic: include/linux/autoconf.h

# Objects we will link into vmlinux / subdirs we need to visit
init-y		:= init/
drivers-y	:= drivers/ sound/
net-y		:= net/
libs-y		:= lib/
core-y		:= usr/
endif # KBUILD_EXTMOD

ifeq ($(dot-config),1)
# In this section, we need .config

# Read in dependencies to all Kconfig* files, make sure to run
# oldconfig if changes are detected.
-include .config.cmd

include .config

# If .config needs to be updated, it will be done via the dependency
# that autoconf has on .config.
# To avoid any implicit rule to kick in, define an empty command
.config: ;

# If .config is newer than include/linux/autoconf.h, someone tinkered
# with it and forgot to run make oldconfig
include/linux/autoconf.h: .config
	$(Q)$(MAKE) -f $(srctree)/Makefile silentoldconfig
else
# Dummy target needed, because used as prerequisite
include/linux/autoconf.h: ;
endif

include $(srctree)/arch/$(ARCH)/Makefile

# Default kernel image to build when no specific target is given.
# KBUILD_IMAGE may be overruled on the commandline or
# set in the environment
# Also any assingments in arch/$(ARCH)/Makefiel take precedence over
# this default value
export KBUILD_IMAGE ?= vmlinux

# The all: target is the default when no target is given on the
# command line.
# This allow a user to issue only 'make' to build a kernel including modules
# Defaults vmlinux but it is usually overriden in the arch makefile
all: vmlinux

ifdef CONFIG_CC_OPTIMIZE_FOR_SIZE
CFLAGS		+= -Os
else
CFLAGS		+= -O2
endif

ifndef CONFIG_FRAME_POINTER
CFLAGS		+= -fomit-frame-pointer
endif

ifdef CONFIG_DEBUG_INFO
CFLAGS		+= -g
endif

# warn about C99 declaration after statement
CFLAGS += $(call check_gcc,-Wdeclaration-after-statement,)

#
# INSTALL_PATH specifies where to place the updated kernel and system map
# images.  Uncomment if you want to place them anywhere other than root.
#

#export	INSTALL_PATH=/boot

#
# INSTALL_MOD_PATH specifies a prefix to MODLIB for module directory
# relocations required by build roots.  This is not defined in the
# makefile but the arguement can be passed to make if needed.
#

MODLIB	:= $(INSTALL_MOD_PATH)/lib/modules/$(KERNELRELEASE)
export MODLIB


ifeq ($(KBUILD_EXTMOD),)
core-y		+= kernel/ mm/ fs/ ipc/ security/ crypto/

vmlinux-dirs	:= $(patsubst %/,%,$(filter %/, $(init-y) $(init-m) \
		     $(core-y) $(core-m) $(drivers-y) $(drivers-m) \
		     $(net-y) $(net-m) $(libs-y) $(libs-m)))

vmlinux-alldirs	:= $(sort $(vmlinux-dirs) $(patsubst %/,%,$(filter %/, \
		     $(init-n) $(init-) \
		     $(core-n) $(core-) $(drivers-n) $(drivers-) \
		     $(net-n)  $(net-)  $(libs-n)    $(libs-))))

init-y		:= $(patsubst %/, %/built-in.o, $(init-y))
core-y		:= $(patsubst %/, %/built-in.o, $(core-y))
drivers-y	:= $(patsubst %/, %/built-in.o, $(drivers-y))
net-y		:= $(patsubst %/, %/built-in.o, $(net-y))
libs-y1		:= $(patsubst %/, %/lib.a, $(libs-y))
libs-y2		:= $(patsubst %/, %/built-in.o, $(libs-y))
libs-y		:= $(libs-y1) $(libs-y2)

# Build vmlinux
# ---------------------------------------------------------------------------

#	This is a bit tricky: If we need to relink vmlinux, we want
#	the version number incremented, which means recompile init/version.o
#	and relink init/init.o. However, we cannot do this during the
#       normal descending-into-subdirs phase, since at that time
#       we cannot yet know if we will need to relink vmlinux.
#	So we descend into init/ inside the rule for vmlinux again.
head-y += $(HEAD)
vmlinux-objs := $(head-y) $(init-y) $(core-y) $(libs-y) $(drivers-y) $(net-y)

quiet_cmd_vmlinux__ = LD      $@
define cmd_vmlinux__
	$(LD) $(LDFLAGS) $(LDFLAGS_vmlinux) $(head-y) $(init-y) \
	--start-group \
	$(core-y) \
	$(libs-y) \
	$(drivers-y) \
	$(net-y) \
	--end-group \
	$(filter .tmp_kallsyms%,$^) \
	-o $@
endef

#	set -e makes the rule exit immediately on error

define rule_vmlinux__
	+set -e;							\
	$(if $(filter .tmp_kallsyms%,$^),,				\
	  echo '  GEN     .version';					\
	  . $(srctree)/scripts/mkversion > .tmp_version;		\
	  mv -f .tmp_version .version;					\
	  $(MAKE) $(build)=init;					\
	)								\
	$(if $($(quiet)cmd_vmlinux__),					\
	  echo '  $($(quiet)cmd_vmlinux__)' &&) 			\
	$(cmd_vmlinux__);						\
	echo 'cmd_$@ := $(cmd_vmlinux__)' > $(@D)/.$(@F).cmd
endef

do_system_map = $(NM) $(1) | grep -v '\(compiled\)\|\(\.o$$\)\|\( [aUw] \)\|\(\.\.ng$$\)\|\(LASH[RL]DI\)' | sort > $(2)

LDFLAGS_vmlinux += -T arch/$(ARCH)/kernel/vmlinux.lds.s

#	Generate section listing all symbols and add it into vmlinux
#	It's a three stage process:
#	o .tmp_vmlinux1 has all symbols and sections, but __kallsyms is
#	  empty
#	  Running kallsyms on that gives us .tmp_kallsyms1.o with
#	  the right size
#	o .tmp_vmlinux2 now has a __kallsyms section of the right size,
#	  but due to the added section, some addresses have shifted
#	  From here, we generate a correct .tmp_kallsyms2.o
#	o The correct .tmp_kallsyms2.o is linked into the final vmlinux.
#	o Verify that the System.map from vmlinux matches the map from
#	  .tmp_vmlinux2, just in case we did not generate kallsyms correctly.
#	o If CONFIG_KALLSYMS_EXTRA_PASS is set, do an extra pass using
#	  .tmp_vmlinux3 and .tmp_kallsyms3.o.  This is only meant as a
#	  temporary bypass to allow the kernel to be built while the
#	  maintainers work out what went wrong with kallsyms.

ifdef CONFIG_KALLSYMS

ifdef CONFIG_KALLSYMS_EXTRA_PASS
last_kallsyms := 3
else
last_kallsyms := 2
endif

kallsyms.o := .tmp_kallsyms$(last_kallsyms).o

define rule_verify_kallsyms
	@$(call do_system_map, .tmp_vmlinux$(last_kallsyms), .tmp_System.map)
	@cmp -s System.map .tmp_System.map || \
		(echo Inconsistent kallsyms data, try setting CONFIG_KALLSYMS_EXTRA_PASS ; rm .tmp_kallsyms* ; false)
endef

quiet_cmd_kallsyms = KSYM    $@
cmd_kallsyms = $(NM) -n $< | $(KALLSYMS) $(foreach x,$(CONFIG_KALLSYMS_ALL),--all-symbols) > $@

.tmp_kallsyms1.o .tmp_kallsyms2.o .tmp_kallsyms3.o: %.o: %.S scripts FORCE
	$(call if_changed_dep,as_o_S)

.tmp_kallsyms%.S: .tmp_vmlinux%
	$(call cmd,kallsyms)

.tmp_vmlinux1: $(vmlinux-objs) arch/$(ARCH)/kernel/vmlinux.lds.s FORCE
	$(call if_changed_rule,vmlinux__)

.tmp_vmlinux2: $(vmlinux-objs) .tmp_kallsyms1.o arch/$(ARCH)/kernel/vmlinux.lds.s FORCE
	$(call if_changed_rule,vmlinux__)

.tmp_vmlinux3: $(vmlinux-objs) .tmp_kallsyms2.o arch/$(ARCH)/kernel/vmlinux.lds.s FORCE
	$(call if_changed_rule,vmlinux__)

endif

#	Finally the vmlinux rule

define rule_vmlinux
	$(rule_vmlinux__); \
	$(call do_system_map, $@, System.map)
	$(rule_verify_kallsyms)
endef

vmlinux: $(vmlinux-objs) $(kallsyms.o) arch/$(ARCH)/kernel/vmlinux.lds.s FORCE
	$(call if_changed_rule,vmlinux)

#	The actual objects are generated when descending, 
#	make sure no implicit rule kicks in

$(sort $(vmlinux-objs)) arch/$(ARCH)/kernel/vmlinux.lds.s: $(vmlinux-dirs) ;

# Handle descending into subdirectories listed in $(vmlinux-dirs)
# Preset locale variables to speed up the build process. Limit locale
# tweaks to this spot to avoid wrong language settings when running
# make menuconfig etc.
# Error messages still appears in the original language

.PHONY: $(vmlinux-dirs)
$(vmlinux-dirs): prepare-all scripts
	$(Q)$(MAKE) $(build)=$@

# Things we need to do before we recursively start building the kernel
# or the modules are listed in "prepare-all".
# A multi level approach is used. prepare1 is updated first, then prepare0.
# prepare-all is the collection point for the prepare targets.

.PHONY: prepare-all prepare prepare0 prepare1 prepare2

# prepare 2 generate Makefile to be placed in output directory, if
# using a seperate output directory. This allows convinient use
# of make in output directory
prepare2:
	$(Q)if [ ! $(srctree) -ef $(objtree) ]; then       \
	$(CONFIG_SHELL) $(srctree)/scripts/mkmakefile      \
	    $(srctree) $(objtree) $(VERSION) $(PATCHLEVEL) \
	    > $(objtree)/Makefile;                         \
	fi

# prepare1 is used to check if we are building in a separate output directory,
# and if so do:
# 1) Check that make has not been executed in the kernel src $(srctree)
# 2) Create the include2 directory, used for the second asm symlink

prepare1: prepare2
ifneq ($(KBUILD_SRC),)
	@echo '  Using $(srctree) as source for kernel'
	$(Q)if [ -h $(srctree)/include/asm -o -f $(srctree)/.config ]; then \
		echo "  $(srctree) is not clean, please run 'make mrproper'";\
		echo "  in the '$(srctree)' directory.";\
		/bin/false; \
	fi;
	$(Q)if [ ! -d include2 ]; then mkdir -p include2; fi;
	$(Q)ln -fsn $(srctree)/include/asm-$(ARCH) include2/asm
endif

prepare0: prepare1 include/linux/version.h include/asm include/config/MARKER
ifneq ($(KBUILD_MODULES),)
	$(Q)rm -rf $(MODVERDIR)
	$(Q)mkdir -p $(MODVERDIR)
endif

# All the preparing..
prepare-all: prepare0 prepare

#	Leave this as default for preprocessing vmlinux.lds.S, which is now
#	done in arch/$(ARCH)/kernel/Makefile

export AFLAGS_vmlinux.lds.o += -P -C -U$(ARCH)

# Single targets
# ---------------------------------------------------------------------------

%.s: %.c scripts FORCE
	$(Q)$(MAKE) $(build)=$(@D) $@
%.i: %.c scripts FORCE
	$(Q)$(MAKE) $(build)=$(@D) $@
%.o: %.c scripts FORCE
	$(Q)$(MAKE) $(build)=$(@D) $@
%/:      scripts prepare FORCE
	$(Q)$(MAKE) KBUILD_MODULES=$(if $(CONFIG_MODULES),1) $(build)=$(@D)
%.lst: %.c scripts FORCE
	$(Q)$(MAKE) $(build)=$(@D) $@
%.s: %.S scripts FORCE
	$(Q)$(MAKE) $(build)=$(@D) $@
%.o: %.S scripts FORCE
	$(Q)$(MAKE) $(build)=$(@D) $@

# 	FIXME: The asm symlink changes when $(ARCH) changes. That's
#	hard to detect, but I suppose "make mrproper" is a good idea
#	before switching between archs anyway.

include/asm:
	@echo '  SYMLINK $@ -> include/asm-$(ARCH)'
	$(Q)if [ ! -d include ]; then mkdir -p include; fi;
	@ln -fsn asm-$(ARCH) $@

# 	Split autoconf.h into include/linux/config/*

include/config/MARKER: include/linux/autoconf.h
	@echo '  SPLIT   include/linux/autoconf.h -> include/config/*'
	@scripts/basic/split-include include/linux/autoconf.h include/config
	@touch $@

# Generate some files
# ---------------------------------------------------------------------------

#	version.h changes when $(KERNELRELEASE) etc change, as defined in
#	this Makefile

uts_len := 64

define filechk_version.h
	if [ `echo -n "$(KERNELRELEASE)" | wc -c ` -gt $(uts_len) ]; then \
	  echo '"$(KERNELRELEASE)" exceeds $(uts_len) characters' >&2; \
	  exit 1; \
	fi; \
	(echo \#define UTS_RELEASE \"$(KERNELRELEASE)\"; \
	  echo \#define LINUX_VERSION_CODE `expr $(VERSION) \\* 65536 + $(PATCHLEVEL) \\* 256 + $(SUBLEVEL)`; \
	 echo '#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))'; \
	)
endef

include/linux/version.h: Makefile
	$(call filechk,version.h)

# ---------------------------------------------------------------------------

.PHONY: depend dep
depend dep:
	@echo '*** Warning: make $@ is unnecessary now.'

# ---------------------------------------------------------------------------
# Modules

ifdef CONFIG_MODULES

# 	By default, build modules as well

all: modules

#	Build modules

.PHONY: modules
modules: $(vmlinux-dirs) $(if $(KBUILD_BUILTIN),vmlinux)
	@echo '  Building modules, stage 2.';
	$(Q)$(MAKE) -rR -f $(srctree)/scripts/Makefile.modpost


# Target to prepare building external modules
.PHONY: modules_prepare
modules_prepare: prepare-all scripts

# Target to install modules
.PHONY: modules_install
modules_install: _modinst_ _modinst_post

.PHONY: _modinst_
_modinst_:
	@if [ -z "`$(DEPMOD) -V | grep module-init-tools`" ]; then \
		echo "Warning: you may need to install module-init-tools"; \
		echo "See http://www.codemonkey.org.uk/docs/post-halloween-2.6.txt";\
		sleep 1; \
	fi
	@rm -rf $(MODLIB)/kernel
	@rm -f $(MODLIB)/source
	@mkdir -p $(MODLIB)/kernel
	@ln -s $(srctree) $(MODLIB)/source
	@if [ ! $(objtree) -ef  $(MODLIB)/build ]; then \
		rm -f $(MODLIB)/build ; \
		ln -s $(objtree) $(MODLIB)/build ; \
	fi
	$(Q)$(MAKE) -rR -f $(srctree)/scripts/Makefile.modinst

# If System.map exists, run depmod.  This deliberately does not have a
# dependency on System.map since that would run the dependency tree on
# vmlinux.  This depmod is only for convenience to give the initial
# boot a modules.dep even before / is mounted read-write.  However the
# boot script depmod is the master version.
ifeq "$(strip $(INSTALL_MOD_PATH))" ""
depmod_opts	:=
else
depmod_opts	:= -b $(INSTALL_MOD_PATH) -r
endif
.PHONY: _modinst_post
_modinst_post: _modinst_
	if [ -r System.map ]; then $(DEPMOD) -ae -F System.map $(depmod_opts) $(KERNELRELEASE); fi

else # CONFIG_MODULES

# Modules not configured
# ---------------------------------------------------------------------------

modules modules_install: FORCE
	@echo
	@echo "The present kernel configuration has modules disabled."
	@echo "Type 'make config' and enable loadable module support."
	@echo "Then build a kernel with module support enabled."
	@echo
	@exit 1

endif # CONFIG_MODULES

# Generate asm-offsets.h 
# ---------------------------------------------------------------------------

define filechk_gen-asm-offsets
	(set -e; \
	 echo "#ifndef __ASM_OFFSETS_H__"; \
	 echo "#define __ASM_OFFSETS_H__"; \
	 echo "/*"; \
	 echo " * DO NOT MODIFY."; \
	 echo " *"; \
	 echo " * This file was generated by arch/$(ARCH)/Makefile"; \
	 echo " *"; \
	 echo " */"; \
	 echo ""; \
	 sed -ne "/^->/{s:^->\([^ ]*\) [\$$#]*\([^ ]*\) \(.*\):#define \1 \2 /* \3 */:; s:->::; p;}"; \
	 echo ""; \
	 echo "#endif" )
endef


###
# Cleaning is done on three levels.
# make clean     Delete most generated files
#                Leave enough to build external modules
# make mrproper  Delete the current configuration, and all generated files
# make distclean Remove editor backup files, patch leftover files and the like

# Directories & files removed with 'make clean'
CLEAN_DIRS  += $(MODVERDIR)
CLEAN_FILES +=	vmlinux System.map \
                .tmp_kallsyms* .tmp_version .tmp_vmlinux* .tmp_System.map

# Directories & files removed with 'make mrproper'
MRPROPER_DIRS  += include/config include2
MRPROPER_FILES += .config .config.old include/asm .version \
                  include/linux/autoconf.h include/linux/version.h \
                  Module.symvers tags TAGS cscope*

# clean - Delete most, but leave enough to build external modules
#
clean: rm-dirs  := $(CLEAN_DIRS)
clean: rm-files := $(CLEAN_FILES)
clean-dirs      := $(addprefix _clean_,$(vmlinux-alldirs))

.PHONY: $(clean-dirs) clean archclean
$(clean-dirs):
	$(Q)$(MAKE) $(clean)=$(patsubst _clean_%,%,$@)

clean: archclean $(clean-dirs)
	$(call cmd,rmdirs)
	$(call cmd,rmfiles)
	@find . $(RCS_FIND_IGNORE) \
	 	\( -name '*.[oas]' -o -name '*.ko' -o -name '.*.cmd' \
		-o -name '.*.d' -o -name '.*.tmp' -o -name '*.mod.c' \) \
		-type f -print | xargs rm -f

# mrproper - Delete all generated files, including .config
#
mrproper: rm-dirs  := $(wildcard $(MRPROPER_DIRS))
mrproper: rm-files := $(wildcard $(MRPROPER_FILES))
mrproper-dirs      := $(addprefix _mrproper_,Documentation/DocBook scripts)

.PHONY: $(mrproper-dirs) mrproper archmrproper
$(mrproper-dirs):
	$(Q)$(MAKE) $(clean)=$(patsubst _mrproper_%,%,$@)

mrproper: clean archmrproper $(mrproper-dirs)
	$(call cmd,rmdirs)
	$(call cmd,rmfiles)

# distclean
#
.PHONY: distclean

distclean: mrproper
	@find $(srctree) $(RCS_FIND_IGNORE) \
	 	\( -name '*.orig' -o -name '*.rej' -o -name '*~' \
		-o -name '*.bak' -o -name '#*#' -o -name '.*.orig' \
	 	-o -name '.*.rej' -o -size 0 \
		-o -name '*%' -o -name '.*.cmd' -o -name 'core' \) \
		-type f -print | xargs rm -f


# Packaging of the kernel to various formats
# ---------------------------------------------------------------------------
# rpm target kept for backward compatibility
package-dir	:= $(srctree)/scripts/package

.PHONY: %-pkg rpm

%pkg: FORCE
	$(Q)$(MAKE) -f $(package-dir)/Makefile $@
rpm: FORCE
	$(Q)$(MAKE) -f $(package-dir)/Makefile $@


# Brief documentation of the typical targets used
# ---------------------------------------------------------------------------

boards := $(wildcard $(srctree)/arch/$(ARCH)/configs/*_defconfig)
boards := $(notdir $(boards))

help:
	@echo  'Cleaning targets:'
	@echo  '  clean		  - remove most generated files but keep the config'
	@echo  '  mrproper	  - remove all generated files + config + various backup files'
	@echo  ''
	@echo  'Configuration targets:'
	@$(MAKE) -f $(srctree)/scripts/kconfig/Makefile help
	@echo  ''
	@echo  'Other generic targets:'
	@echo  '  all		  - Build all targets marked with [*]'
	@echo  '* vmlinux	  - Build the bare kernel'
	@echo  '* modules	  - Build all modules'
	@echo  '  modules_install - Install all modules'
	@echo  '  dir/            - Build all files in dir and below'
	@echo  '  dir/file.[ois]  - Build specified target only'
	@echo  '  rpm		  - Build a kernel as an RPM package'
	@echo  '  tags/TAGS	  - Generate tags file for editors'
	@echo  '  cscope	  - Generate cscope index'
	@echo  '  checkstack      - Generate a list of stack hogs'
	@echo  'Kernel packaging:'
	@$(MAKE) -f $(package-dir)/Makefile help
	@echo  ''
	@echo  'Documentation targets:'
	@$(MAKE) -f $(srctree)/Documentation/DocBook/Makefile dochelp
	@echo  ''
	@echo  'Architecture specific targets ($(ARCH)):'
	@$(if $(archhelp),$(archhelp),\
		echo '  No architecture specific help defined for $(ARCH)')
	@echo  ''
	@$(if $(boards), \
		$(foreach b, $(boards), \
		printf "  %-24s - Build for %s\\n" $(b) $(subst _defconfig,,$(b));) \
		echo '')

	@echo  '  make V=0|1 [targets] 0 => quiet build (default), 1 => verbose build'
	@echo  '  make O=dir [targets] Locate all output files in "dir", including .config'
	@echo  '  make C=1   [targets] Check all c source with checker tool'
	@echo  ''
	@echo  'Execute "make" or "make all" to build all targets marked with [*] '
	@echo  'For further info see the ./README file'


# Documentation targets
# ---------------------------------------------------------------------------
%docs: scripts_basic FORCE
	$(Q)$(MAKE) $(build)=Documentation/DocBook $@

else # KBUILD_EXTMOD

###
# External module support.
# When building external modules the kernel used as basis is considered
# read-only, and no consistency checks are made and the make
# system is not used on the basis kernel. If updates are required
# in the basis kernel ordinary make commands (without M=...) must
# be used.
#
# The following are the only valid targets when building external
# modules.
# make M=dir clean     Delete all automatically generated files
# make M=dir modules   Make all modules in specified dir
# make M=dir	       Same as 'make M=dir modules'
# make M=dir modules_install
#                      Install the modules build in the module directory
#                      Assumes install directory is already created

# We are always building modules
KBUILD_MODULES := 1
.PHONY: crmodverdir
crmodverdir:
	$(Q)mkdir -p $(MODVERDIR)

module-dirs := $(addprefix _module_,$(KBUILD_EXTMOD))
.PHONY: $(module-dirs) modules
$(module-dirs): crmodverdir
	$(Q)$(MAKE) $(build)=$(patsubst _module_%,%,$@)

modules: $(module-dirs)
	@echo '  Building modules, stage 2.';
	$(Q)$(MAKE) -rR -f $(srctree)/scripts/Makefile.modpost

.PHONY: modules_install
modules_install:
	$(Q)$(MAKE) -rR -f $(srctree)/scripts/Makefile.modinst

clean-dirs := $(addprefix _clean_,$(KBUILD_EXTMOD))

.PHONY: $(clean-dirs) clean
$(clean-dirs):
	$(Q)$(MAKE) $(clean)=$(patsubst _clean_%,%,$@)

clean:	rm-dirs := $(MODVERDIR)
clean: $(clean-dirs)
	$(call cmd,rmdirs)
	@find $(KBUILD_EXTMOD) $(RCS_FIND_IGNORE) \
	 	\( -name '*.[oas]' -o -name '*.ko' -o -name '.*.cmd' \
		-o -name '.*.d' -o -name '.*.tmp' -o -name '*.mod.c' \) \
		-type f -print | xargs rm -f

help:
	@echo  '  Building external modules.'
	@echo  '  Syntax: make -C path/to/kernel/src M=$$PWD target'
	@echo  ''
	@echo  '  modules         - default target, build the module(s)'
	@echo  '  modules_install - install the module'
	@echo  '  clean           - remove generated files in module directory only'
	@echo  ''
endif # KBUILD_EXTMOD

# Generate tags for editors
# ---------------------------------------------------------------------------

define all-sources
	( find $(srctree) $(RCS_FIND_IGNORE) \
	       \( -name include -o -name arch \) -prune -o \
	       -name '*.[chS]' -print; \
	  find $(srctree)/arch/$(ARCH) $(RCS_FIND_IGNORE) \
	       -name '*.[chS]' -print; \
	  find $(srctree)/security/selinux/include $(RCS_FIND_IGNORE) \
	       -name '*.[chS]' -print; \
	  find $(srctree)/include $(RCS_FIND_IGNORE) \
	       \( -name config -o -name 'asm-*' \) -prune \
	       -o -name '*.[chS]' -print; \
	  find $(srctree)/include/asm-$(ARCH) $(RCS_FIND_IGNORE) \
	       -name '*.[chS]' -print; \
	  find $(srctree)/include/asm-generic $(RCS_FIND_IGNORE) \
	       -name '*.[chS]' -print )
endef

quiet_cmd_cscope-file = FILELST cscope.files
      cmd_cscope-file = $(all-sources) > cscope.files

quiet_cmd_cscope = MAKE    cscope.out
      cmd_cscope = cscope -k -b -q

cscope: FORCE
	$(call cmd,cscope-file)
	$(call cmd,cscope)

quiet_cmd_TAGS = MAKE   $@
cmd_TAGS = $(all-sources) | etags -

# 	Exuberant ctags works better with -I

quiet_cmd_tags = MAKE   $@
define cmd_tags
	rm -f $@; \
	CTAGSF=`ctags --version | grep -i exuberant >/dev/null && echo "-I __initdata,__exitdata,EXPORT_SYMBOL,EXPORT_SYMBOL_NOVERS"`; \
	$(all-sources) | xargs ctags $$CTAGSF -a
endef

TAGS: FORCE
	$(call cmd,TAGS)

tags: FORCE
	$(call cmd,tags)


# Scripts to check various things for consistency
# ---------------------------------------------------------------------------

configcheck:
	find * $(RCS_FIND_IGNORE) \
		-name '*.[hcS]' -type f -print | sort \
		| xargs $(PERL) -w scripts/checkconfig.pl

includecheck:
	find * $(RCS_FIND_IGNORE) \
		-name '*.[hcS]' -type f -print | sort \
		| xargs $(PERL) -w scripts/checkincludes.pl

versioncheck:
	find * $(RCS_FIND_IGNORE) \
		-name '*.[hcS]' -type f -print | sort \
		| xargs $(PERL) -w scripts/checkversion.pl

buildcheck:
	$(PERL) scripts/reference_discarded.pl
	$(PERL) scripts/reference_init.pl

endif #ifeq ($(config-targets),1)
endif #ifeq ($(mixed-targets),1)

.PHONY: checkstack
checkstack:
	$(OBJDUMP) -d vmlinux $$(find . -name '*.ko') | \
	$(PERL) $(src)/scripts/checkstack.pl $(ARCH)

# FIXME Should go into a make.lib or something 
# ===========================================================================

quiet_cmd_rmdirs = $(if $(wildcard $(rm-dirs)),CLEAN   $(wildcard $(rm-dirs)))
      cmd_rmdirs = rm -rf $(rm-dirs)

quiet_cmd_rmfiles = $(if $(wildcard $(rm-files)),CLEAN   $(wildcard $(rm-files)))
      cmd_rmfiles = rm -f $(rm-files)


a_flags = -Wp,-MD,$(depfile) $(AFLAGS) $(AFLAGS_KERNEL) \
	  $(NOSTDINC_FLAGS) $(CPPFLAGS) \
	  $(modkern_aflags) $(EXTRA_AFLAGS) $(AFLAGS_$(*F).o)

quiet_cmd_as_o_S = AS      $@
cmd_as_o_S       = $(CC) $(a_flags) -c -o $@ $<

# read all saved command lines

targets := $(wildcard $(sort $(targets)))
cmd_files := $(wildcard .*.cmd $(foreach f,$(targets),$(dir $(f)).$(notdir $(f)).cmd))

ifneq ($(cmd_files),)
  $(cmd_files): ;	# Do not try to update included dependency files
  include $(cmd_files)
endif

# execute the command and also postprocess generated .d dependencies
# file

if_changed_dep = $(if $(strip $? $(filter-out FORCE $(wildcard $^),$^)\
		          $(filter-out $(cmd_$(1)),$(cmd_$@))\
			  $(filter-out $(cmd_$@),$(cmd_$(1)))),\
	@set -e; \
	$(if $($(quiet)cmd_$(1)),echo '  $(subst ','\'',$($(quiet)cmd_$(1)))';) \
	$(cmd_$(1)); \
	scripts/basic/fixdep $(depfile) $@ '$(subst $$,$$$$,$(subst ','\'',$(cmd_$(1))))' > $(@D)/.$(@F).tmp; \
	rm -f $(depfile); \
	mv -f $(@D)/.$(@F).tmp $(@D)/.$(@F).cmd)

# Usage: $(call if_changed_rule,foo)
# will check if $(cmd_foo) changed, or any of the prequisites changed,
# and if so will execute $(rule_foo)

if_changed_rule = $(if $(strip $? \
		               $(filter-out $(cmd_$(1)),$(cmd_$(@F)))\
			       $(filter-out $(cmd_$(@F)),$(cmd_$(1)))),\
	               @$(rule_$(1)))

# If quiet is set, only print short version of command

cmd = @$(if $($(quiet)cmd_$(1)),echo '  $($(quiet)cmd_$(1))' &&) $(cmd_$(1))

# filechk is used to check if the content of a generated file is updated.
# Sample usage:
# define filechk_sample
#	echo $KERNELRELEASE
# endef
# version.h : Makefile
#	$(call filechk,sample)
# The rule defined shall write to stdout the content of the new file.
# The existing file will be compared with the new one.
# - If no file exist it is created
# - If the content differ the new file is used
# - If they are equal no change, and no timestamp update

define filechk
	@set -e;				\
	echo '  CHK     $@';			\
	mkdir -p $(dir $@);			\
	$(filechk_$(1)) < $< > $@.tmp;		\
	if [ -r $@ ] && cmp -s $@ $@.tmp; then	\
		rm -f $@.tmp;			\
	else					\
		echo '  UPD     $@';		\
		mv -f $@.tmp $@;		\
	fi
endef

# Shorthand for $(Q)$(MAKE) -f scripts/Makefile.build obj=dir
# Usage:
# $(Q)$(MAKE) $(build)=dir
build := -f $(if $(KBUILD_SRC),$(srctree)/)scripts/Makefile.build obj

# Shorthand for $(Q)$(MAKE) -f scripts/Makefile.clean obj=dir
# Usage:
# $(Q)$(MAKE) $(clean)=dir
clean := -f $(if $(KBUILD_SRC),$(srctree)/)scripts/Makefile.clean obj

#	$(call descend,<dir>,<target>)
#	Recursively call a sub-make in <dir> with target <target>
# Usage is deprecated, because make does not see this as an invocation of make.
descend =$(Q)$(MAKE) -f $(if $(KBUILD_SRC),$(srctree)/)scripts/Makefile.build obj=$(1) $(2)

endif	# skip-makefile

FORCE:
