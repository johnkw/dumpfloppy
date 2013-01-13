AC_PREREQ(2.59)
AC_INIT(dumpfloppy, 1, ats@offog.org)
AC_CONFIG_SRCDIR([dumpfloppy.c])

AM_INIT_AUTOMAKE([foreign])

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL

AC_CHECK_HEADERS([linux/fd.h linux/fdreg.h], ,
    [AC_MSG_ERROR([This tool requires the Linux floppy interface.])])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
