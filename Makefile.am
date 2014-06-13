bin_PROGRAMS = \
	dumpfloppy \
	imdcat

common_sources = \
	disk.cpp \
	disk.h \
	imd.cpp \
	imd.h \
	show.cpp \
	show.h \
	util.cpp \
	util.h

dumpfloppy_SOURCES = \
	$(common_sources) \
	dumpfloppy.cpp

imdcat_SOURCES = \
	$(common_sources) \
	imdcat.cpp

AM_CPPFLAGS = \
	-Wall -Werror -Wextra -g -O0
