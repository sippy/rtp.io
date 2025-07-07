#!/bin/sh

set -e

platformopts() {
  case "${TARGETPLATFORM}" in
  linux/arm64)
    echo "QEMU_CPU=cortex-a53"
    ;;
  linux/386)
    echo "OPENSSL_CONFIGURE_ARGS=linux-x86"
    ;;
  esac
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
