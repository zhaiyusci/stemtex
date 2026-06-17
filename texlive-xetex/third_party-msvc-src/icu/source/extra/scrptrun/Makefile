## Copyright (C) 2016 and later: Unicode, Inc. and others.
## License & terms of use: http://www.unicode.org/copyright.html
## Makefile.in for ICU - extra/scrptrun
## Copyright (c) 2001-2011, International Business Machines Corporation and
## others. All Rights Reserved.

## Source directory information
srcdir = .
top_srcdir = ../..

top_builddir = ../..

include $(top_builddir)/icudefs.mk

## Build directory information
subdir = extra/scrptrun

## Extra files to remove for 'make clean'
CLEANFILES = *~ $(DEPS)

## Target information
TARGET = srtest$(EXEEXT)

DEFS = -DPACKAGE_NAME=\"ICU\" -DPACKAGE_TARNAME=\"icu4c\" -DPACKAGE_VERSION=\"78.3\" -DPACKAGE_STRING=\"ICU\ 78.3\" -DPACKAGE_BUGREPORT=\"https://icu.unicode.org/bugs\" -DPACKAGE_URL=\"https://icu.unicode.org/\" -DHAVE_STDIO_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRING_H=1 -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DSTDC_HEADERS=1 -DSIZEOF_VOID_P=8 -DHAVE_LIBM=1 -DHAVE_WCHAR_H=1 -DSIZEOF_WCHAR_T=2
CPPFLAGS =  $(THREADSCPPFLAGS)  -DHAVE_DLOPEN=0 -DU_HAVE_MMAP=0 -DU_HAVE_DIRENT_H=0 -DU_HAVE_POPEN=0 -DU_HAVE_TZNAME=0  -I$(top_srcdir)/common -I$(top_srcdir) 
CFLAGS = -Gy -MD -Gy -MT $(THREADSCFLAGS) 
CXXFLAGS = -Gy -MD -Gy -MT /std:c++17 $(THREADSCXXFLAGS) 
ENABLE_RPATH = NO
ifeq ($(ENABLE_RPATH),YES)
RPATHLDFLAGS = $(LD_RPATH)$(LD_RPATH_PRE)$(libdir)
endif
LDFLAGS =  $(RPATHLDFLAGS)
LIBS = $(LIBICUUC) $(LIB_M)

SOURCES = $(shell cat $(srcdir)/sources.txt)
OBJECTS = $(SOURCES:.cpp=.o)

DEPS = $(OBJECTS:.o=.d)

## List of phony targets
.PHONY : all all-local install install-local clean clean-local	\
distclean distclean-local dist dist-local check check-local

## Clear suffix list
.SUFFIXES :

## List of standard targets
all: all-local
install: install-local
clean: clean-local
distclean : distclean-local
dist: dist-local
check: all check-local

all-local: $(TARGET)

install-local:

dist-local:

clean-local:
	test -z "$(CLEANFILES)" || $(RMV) $(CLEANFILES)
	$(RMV) $(OBJECTS) $(TARGET)

distclean-local: clean-local
	$(RMV) Makefile

check-local: all-local

Makefile: $(srcdir)/Makefile.in  $(top_builddir)/config.status
	cd $(top_builddir) \
	 && CONFIG_FILES=$(subdir)/$@ CONFIG_HEADERS= $(SHELL) ./config.status

$(TARGET) : $(OBJECTS)
	$(LINK.cc) $(OUTOPT)$@ $^ $(LIBS)
	$(POST_BUILD_STEP)

invoke:
	ICU_DATA=$${ICU_DATA:-$(top_builddir)/data/} TZ=PST8PDT $(INVOKE) $(INVOCATION)

ifeq (,$(MAKECMDGOALS))
-include $(DEPS)
else
ifneq ($(patsubst %clean,,$(MAKECMDGOALS)),)
-include $(DEPS)
endif
endif
