AC_INIT([component], [2.72])
AM_INIT_AUTOMAKE([foreign subdir-objects])
LT_INIT
AC_PROG_CC
AC_PROG_CXX

AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug],[Enable debug mode])],
  [],
  [enable_debug=yes])

AM_CONDITIONAL([DEBUG], [test "x$enable_debug" = "xyes"])
AS_IF([test "x$enable_debug" = "xyes"], [
    AC_DEFINE([DEBUG], [1], [Enable debug mode])
])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

