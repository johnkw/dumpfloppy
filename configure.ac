AC_PREREQ(2.59)
AC_INIT(dumpfloppy, 2, ats@offog.org)
${CXXFLAGS=""}
AC_CONFIG_SRCDIR([dumpfloppy.cpp])

AM_INIT_AUTOMAKE([foreign])

# Checks for programs.
AC_PROG_CXX
AC_PROG_INSTALL

AC_CHECK_HEADERS([linux/fd.h linux/fdreg.h], ,
    [AC_MSG_ERROR([This tool requires the Linux floppy interface.])])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
