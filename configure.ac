dnl Process this file with autoconf to produce a configure script.

AC_INIT(gstfs.c)
AM_INIT_AUTOMAKE(gstfs, 0.2)

AC_ISC_POSIX
AC_PROG_CC
AM_PROG_CC_STDC
AC_HEADER_STDC

ACX_PTHREAD(, AC_MSG_ERROR(This program requires POSIX threads))
PKG_CHECK_MODULES([gstfs], [glib-2.0 gstreamer-0.10 fuse])
AC_DEFINE(FUSE_USE_VERSION,26,[Fuse API Version])

AC_OUTPUT(Makefile)
