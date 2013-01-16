bin_PROGRAMS = \
	dumpfloppy \
	imdcat

common_sources = \
	disk.c \
	disk.h \
	imd.c \
	imd.h \
	show.c \
	show.h \
	util.c \
	util.h

dumpfloppy_SOURCES = \
	$(common_sources) \
	dumpfloppy.c

imdcat_SOURCES = \
	$(common_sources) \
	imdcat.c

AM_CPPFLAGS = \
	-std=gnu99 -Wall -g
