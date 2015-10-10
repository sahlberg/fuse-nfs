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


AC_HEADER_ASSERT
AC_CHECK_HEADER([fuse.h], [], [AC_MSG_ERROR([fuse.h is missing. You need to install libfuse-dev])], [])
AC_CHECK_HEADER([nfsc/libnfs.h], [], [AC_MSG_ERROR([libnfs.h is missing. You need to install libnfs-dev])], [])

#output
AC_CONFIG_FILES([Makefile]
                [doc/Makefile]
                [fuse/Makefile]
               )

AC_OUTPUT([fuse-nfs.pc])
