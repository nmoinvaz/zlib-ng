#!/bin/sh

usage() {
  cat <<"_EOF_"
Usage: sh test/pkgcheck.sh [--zlib-compat]

Verifies that the various build systems produce identical results on a Unixlike system.
Verifies the resulting shared library is ABI-compatible with the reference version.
If --zlib-compat, tests with zlib compatible builds.

To cross-build, install the appropriate qemu and gcc packages,
and set the environment variables used by configure or cmake, e.g.

armel:
$ sudo apt install ninja-build diffoscope qemu gcc-arm-linux-gnueabihf libc-dev-armel-cross
$ export CHOST=arm-linux-gnueabihf
$ export CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm.cmake -DCMAKE_C_COMPILER_TARGET=${CHOST}"

aarch64:
$ sudo apt install ninja-build diffoscope qemu gcc-aarch64-linux-gnu libc-dev-arm64-cross
$ export CHOST=aarch64-linux-gnu
$ export CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake -DCMAKE_C_COMPILER_TARGET=${CHOST}"

ppc:
$ sudo apt install ninja-build diffoscope qemu gcc-powerpc-linux-gnu libc-dev-powerpc-cross
$ export CHOST=powerpc-linux-gnu
$ export CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-powerpc.cmake"

then:
$ export CC=${CHOST}-gcc
$ sh test/pkgcheck.sh [--zlib-compat]
_EOF_
}

set -ex

# Caller can also set CMAKE_ARGS or CONFIGURE_ARGS if desired
CMAKE_ARGS=${CMAKE_ARGS}
CONFIGURE_ARGS=${CONFIGURE_ARGS}

case "$1" in
--zlib-compat)
  suffix=""
  CMAKE_ARGS="$CMAKE_ARGS -DZLIB_COMPAT=ON"
  CONFIGURE_ARGS="$CONFIGURE_ARGS --zlib-compat"
  ;;
"")
  suffix="-ng"
  ;;
*)
  echo "Unknown arg '$1'"
  usage
  exit 1
  ;;
esac

if ! test -f "configure"
then
  echo "Please run from top of source tree"
  exit 1
fi

# Tell GNU's ld etc. to use Jan 1 1970 when embedding timestamps
# Probably only needed on older systems (ubuntu 14.04?)
#export SOURCE_DATE_EPOCH=0
# Tell Apple's ar etc. to use zero timestamps
export ZERO_AR_DATE=1

# Use same compiler for make and cmake builds
if test "$CC"x = ""x
then
  if clang --version
  then
    export CC=clang
  elif gcc --version
  then
    export CC=gcc
  fi
fi

# New build system
# Happens to delete top-level zconf.h
# (which itself is a bug, https://github.com/madler/zlib/issues/162 )
# which triggers another bug later in configure,
# https://github.com/madler/zlib/issues/499
rm -rf btmp2 pkgtmp2
mkdir btmp2 pkgtmp2
export DESTDIR=$(pwd)/pkgtmp2
cd btmp2
  cmake -G Ninja ${CMAKE_ARGS} ..
  ninja -v
  ninja install
cd ..

# Original build system
rm -rf btmp1 pkgtmp1
mkdir btmp1 pkgtmp1
export DESTDIR=$(pwd)/pkgtmp1
cd btmp1
  case $(uname) in
  Darwin)
    export LDFLAGS="-Wl,-headerpad_max_install_names"
    ;;
  esac
  # Use same optimization level as cmake did.
  CFLAGS="$(awk -F= '/CMAKE_C_FLAGS_RELEASE:STRING=/ {print $2}' < ../btmp2/CMakeCache.txt)"
  export CFLAGS
  # Hack: oddly, given CHOST=powerpc-linux-gnu, configure concludes uname is gnu,
  # causing it to set LDSHAREDFLAGS to a different value than cmake uses.
  # So override it when appropriate.  FIXME
  case "$CHOST" in
  *linux*) bash ../configure --uname=linux $CONFIGURE_ARGS;;
  *)       bash ../configure $CONFIGURE_ARGS;;
  esac
  make
  make install
cd ..

repack_ar() {
  if ! cmp --silent pkgtmp1/usr/local/lib/libz$suffix.a pkgtmp2/usr/local/lib/libz$suffix.a
  then
    echo "Warning: libz$suffix.a does not match.  Assuming ar needs -D option.  Unpacking..."
    cd pkgtmp1; ar x usr/local/lib/libz$suffix.a; rm usr/local/lib/libz$suffix.a; cd ..
    cd pkgtmp2; ar x usr/local/lib/libz$suffix.a; rm usr/local/lib/libz$suffix.a; for a in *.c.o; do b=$(echo $a | sed 's/\..*//'); mv $a $b.o; done; cd ..
  fi
}

case $(uname) in
Darwin)
  # Alas, dylibs still have an embedded hash or something,
  # so nuke it.
  # FIXME: find a less fragile way to deal with this.
  dylib1=$(find pkgtmp1 -type f -name '*.dylib*')
  dylib2=$(find pkgtmp2 -type f -name '*.dylib*')
  dd conv=notrunc if=/dev/zero of=$dylib1 skip=1337 count=16
  dd conv=notrunc if=/dev/zero of=$dylib2 skip=1337 count=16
  ;;
FreeBSD|Linux)
  # The ar on newer systems defaults to -D (i.e. deterministic),
  # but FreeBSD 12.1, Debian 8, and Ubuntu 14.04 seem to not do that.
  # I had trouble passing -D safely to the ar inside CMakeLists.txt,
  # so punt and unpack the archive if needed before comparing.
  repack_ar
  ;;
esac

if diff -Nur pkgtmp1 pkgtmp2
then
  echo pkgcheck-cmake-bits-identical PASS
else
  echo pkgcheck-cmake-bits-identical FAIL
  dylib1=$(find pkgtmp1 -type f -name '*.dylib*' -print -o -type f -name '*.so.*' -print)
  dylib2=$(find pkgtmp2 -type f -name '*.dylib*' -print -o -type f -name '*.so.*' -print)
  diffoscope $dylib1 $dylib2 | cat
  exit 1
fi

# Print the multiarch tuple for the current (non-cross) machine, or the empty string if unavailable.
detect_chost() {
    dpkg-architecture -qDEB_HOST_MULTIARCH ||
     $CC -print-multiarch ||
     $CC -print-search-dirs | sed 's/:/\n/g' | grep -E '^/lib/[^/]+$' | sed 's%.*/%%' ||
     true
}

# Test compat build for ABI compatibility with zlib
ABIFILE=""
if test "$CHOST" = ""
then
  CHOST=$(detect_chost)
fi
if test "$CHOST" = ""
then
  echo "pkgcheck-abi-check SKIP, as we don't know CHOST"
else
  if test "$suffix" = ""
  then
    # Reference is zlib 1.2.11
    MAYBE_ABIFILE="test/zlib$suffix-1.2.11-$CHOST.abi"
  else
    # Reference is zlib-ng 2.0 (well, '1.9.9' for now)
    MAYBE_ABIFILE="test/zlib$suffix-1.9.9-$CHOST.abi"
  fi
  if test -f "$MAYBE_ABIFILE"
  then
    ABIFILE="$MAYBE_ABIFILE"
  else
    echo "pkgcheck-abi-check SKIP, as $MAYBE_ABIFILE does not exist in git; FIXME: could check out zlib source and build it here"
  fi
fi
if test "$ABIFILE" != ""
then
  abidw --version
  # Use unstripped shared library in btmp, not stripped one in pkgtmp1
  dylib1=$(find btmp1 -type f -name '*.dylib*' -print -o -type f -name '*.so.*' -print)
  abidw $dylib1 > zlib${suffix}-built.abi
  # Don't complain about new variables or functions, since zlib-ng does define new ones.
  # FIXME: use --no-added-syms for now, but we probably want to be more strict.
  # In compat mode, should we declare ng-ish symbols ZLIB_INTERNAL ?
  if abidiff "$ABIFILE" zlib${suffix}-built.abi
  then
    echo "pkgcheck-abi-check PASS"
  else
    echo "pkgcheck-abi-check FAIL"
    exit 1
  fi
fi
# any failure would have caused an early exit already
echo "pkgcheck: PASS"
