#!/bin/sh

set -e

platformopts() {
  case "${TARGETPLATFORM}" in
  linux/arm64)
    echo "QEMU_CPU=cortex-a53"
    ;;
  linux/amd64)
    echo "CCACHE_COMPRESS=true"
    echo "CCACHE_MAXSIZE=40M"
    ;;
  linux/386)
    echo "OPENSSL_CONFIGURE_ARGS=linux-x86"
    echo "CCACHE_COMPRESS=true"
    echo "CCACHE_MAXSIZE=40M"
    ;;
  esac
  if [ -e /opt/rh/gcc-toolset-14/root/usr/lib/gcc/i686-redhat-linux/14/libatomic.a ]
  then
    echo "SRTP_LIBS=-L/opt/rh/gcc-toolset-14/root/usr/lib/gcc/i686-redhat-linux/14 -l:libatomic.a"
  fi
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
