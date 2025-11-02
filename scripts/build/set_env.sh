#!/bin/sh

set -e

get_cc_ver() {
  CC_v="`${CC} -v 2>&1`"
  if [ "${?}" -ne 0 -o ! -n "${CC_v}" ]
  then
    exit 1
  fi
  CC_dm="`${CC} -dumpmachine 2>&1`"
  if [ "${?}" -ne 0 -o ! -n "${CC_dm}" ]
  then
    exit 1
  fi
  CC_dv="`${CC} -dumpversion 2>&1`"
  if [ "${?}" -ne 0 -o ! -n "${CC_dv}" ]
  then
    exit 1
  fi
  echo "${CC_v}${CC_dm}${CC_dv}" | md5sum | awk '{print $1}'
}

platformopts() {
  case "${TARGETPLATFORM}" in
  linux/arm64)
    echo "QEMU_CPU=cortex-a53"
    ;;
  linux/amd64 | linux/386)
    echo "CCACHE_COMPRESS=true"
    echo "CCACHE_MAXSIZE=40M"
    ;;
  esac
  if [ -e /opt/rh/gcc-toolset-14/root/usr/lib/gcc/i686-redhat-linux/14/libatomic.a ]
  then
    echo "SRTP_LIBS=-L/opt/rh/gcc-toolset-14/root/usr/lib/gcc/i686-redhat-linux/14 -l:libatomic.a"
  fi
  CC_VER="`get_cc_ver`"
  if [ "${?}" -ne 0 -o ! -n "${CC_VER}" -o "${CC_VER}" = "68b329da9893e34099c7d8ad5cb9c940" ]
  then
    exit 1
  fi
  echo "CCACHE_COMPILERCHECK=string:${CC_VER}"
  echo "SAVE_SPACE=yes"
}

case "${1}" in
platformopts)
  shift
  platformopts "${@}"
  ;;
*)
  echo "usage: `basename "${0}"` platformopts [opts]" 2>&1
  exit 1
  ;;
esac
