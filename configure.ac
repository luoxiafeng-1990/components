AC_INIT([component], [1.0])
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

# Check for liburing library
AC_CHECK_HEADER([liburing.h], [], [
    AC_MSG_ERROR([liburing.h not found. Please install liburing development package.])
])
AC_CHECK_LIB([uring], [io_uring_queue_init], [], [
    AC_MSG_ERROR([liburing library not found. Please install liburing development package.])
])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

