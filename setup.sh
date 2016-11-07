#!/bin/sh

set -e

libtoolize
aclocal
autoheader
autoconf
automake --add-missing
