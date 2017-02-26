AC_PREREQ(2.50)
AC_INIT([fuse-nfs], [1.0.0], [ronniesahlberg@gmail.com])
AM_INIT_AUTOMAKE([-Wall foreign])
AC_CANONICAL_HOST
AC_CONFIG_MACRO_DIR([m4])
m4_pattern_allow([AM_PROG_AR])
AM_PROG_AR

# Work around stupid autoconf default cflags. pt 1
SAVE_CFLAGS="x${CFLAGS}"

AC_PROG_CC
AC_PROG_LIBTOOL

AM_PROG_CC_C_O
PKG_PROG_PKG_CONFIG

# Work around stupid autoconf default cflags. pt 2
if test "$SAVE_CFLAGS" = "x"; then
  CFLAGS=""
fi

# We always want 64 bit file offsets
CFLAGS="${CFLAGS} -D_FILE_OFFSET_BITS=64"

if test "$ac_cv_prog_gcc" = yes; then
   WARN_CFLAGS="-Wall -Werror -Wshadow -Wno-write-strings -Wstrict-prototypes -Wpointer-arith -Wcast-align -Wno-strict-aliasing"
fi
AC_SUBST(WARN_CFLAGS)

AC_CONFIG_HEADER(config.h)

AC_HEADER_ASSERT
AC_CHECK_HEADER([fuse.h], [], [AC_MSG_ERROR([fuse.h is missing. You need to install libfuse-dev])], [])
AC_CHECK_HEADER([nfsc/libnfs.h], [], [AC_MSG_ERROR([libnfs.h is missing. You need to install libnfs-dev])], [])

AC_CHECK_HEADERS([fuse.h])

AC_CACHE_CHECK([for st_atim support],libiscsi_cv_HAVE_ST_ATIM,[
AC_TRY_COMPILE([
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>],
[struct stat st; st.st_atim.tv_sec = 0;],
libiscsi_cv_HAVE_ST_ATIM=yes,libiscsi_cv_HAVE_ST_ATIM=no)])
if test x"$libiscsi_cv_HAVE_ST_ATIM" = x"yes"; then
    AC_DEFINE(HAVE_ST_ATIM,1,[Whether we have st_atim support])
fi

AC_SEARCH_LIBS([fuse_get_context], [fuse dokanfuse1.dll], [], [
  AC_MSG_ERROR([fuse library unavailable])
])

OLD_LIBS=$LIBS
LIBS+=" -lws2"
AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <winsock2.h>]], [[gethostbyname ("");]])], [], [LIBS=$OLD_LIBS])
OLD_LIBS=$LIBS
LIBS+=" -lws2_32"
AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <winsock2.h>]], [[gethostbyname ("");]])], [], [LIBS=$OLD_LIBS])

#output
AC_CONFIG_FILES([Makefile]
                [doc/Makefile]
                [fuse/Makefile]
               )

AC_OUTPUT([fuse-nfs.pc])
