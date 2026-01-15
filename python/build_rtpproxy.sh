#!/bin/sh

set -e

_SELF="`realpath "${0}"`"
MYDIR="`dirname ${_SELF}`"

BDIR="${MYDIR}/../build"

if ${CC} -v 2>&1 | grep -q "clang"
then
  export AR="llvm-ar"
  export RANLIB="llvm-ranlib"
fi
if [ "`uname -o`" = "FreeBSD" ]
then
  GMAKE=gmake
else
  GMAKE=make
fi

CFLAGS_opt="-O1 -pipe -flto"
CFLAGS="-O0 -g3 -pipe -flto"
if [ ! -z "${ARCH_CFLAGS}" ]
then
  CFLAGS_opt="${CFLAGS_opt} ${ARCH_CFLAGS}"
  CFLAGS="${CFLAGS} ${ARCH_CFLAGS}"
fi
LDFLAGS_opt="-O2 -flto ${LDFLAGS}"
LDFLAGS="-O0 -flto ${LDFLAGS}"

pv_run() {
  local LOGFILE="`mktemp`"
  if ! "${@}" 2>&1 | \
   pv -fcl -F "%t %b %p" -i 60 2>&1 1>"${LOGFILE}" | tr '\r' '\n' 1>&2
  then
    tail -n 200 "${LOGFILE}"
    rm "${LOGFILE}"
    return 1
  fi
}

if pv -V 2>/dev/null
then
  RUN_LOG=pv_run
else
  RUL_LOG=""
fi

if [ -z "${OPENSSL_CONFIGURE_ARGS}" ]
then
  if which rpm > /dev/null
  then
    ARCH="`rpm --eval '%{_arch}'`"
  fi
  if which dpkg > /dev/null
  then
    ARCH="`dpkg --print-architecture`"
  fi
  if [ "${ARCH}" = "i386" ]
  then
    OPENSSL_CONFIGURE_ARGS="linux-x86"
    if [ -z "${SRTP_LIBS}" ]
    then
      export LIB_CRYPTO_STATIC="-l:libcrypto.a -lpthread -latomic"
    else
      export LIB_CRYPTO_STATIC="-l:libcrypto.a -lpthread ${SRTP_LIBS}"
    fi
  fi
fi

if [ -z "${NO_BUILD_OSSL}" ]
then
  cd ${MYDIR}/../openssl
  CFLAGS="${CFLAGS_opt}" LDFLAGS="${LDFLAGS_opt}" ./Configure ${OPENSSL_CONFIGURE_ARGS} \
   no-shared no-module no-dso \
   no-tests no-apps no-unit-test no-quic no-docs --prefix="${BDIR}" --libdir=lib
  ${RUN_LOG} make all
  make install_sw
fi

CFLAGS_opt="${CFLAGS_opt} -I${BDIR}/include"
CFLAGS="${CFLAGS} -I${BDIR}/include"
LDFLAGS_opt="${LDFLAGS_opt} -L${BDIR}/lib"
LDFLAGS="${LDFLAGS} -L${BDIR}/lib"

if [ -z "${NO_BUILD_SRTP}" ]
then
  cd ${MYDIR}/../libsrtp
  if [ ! -z "${SAVE_SPACE}" ]
  then
    rm -rf ${MYDIR}/../openssl
  fi
  if ! CFLAGS="${CFLAGS_opt}" LDFLAGS="${LDFLAGS_opt}" LIBS="${SRTP_LIBS}" \
   ./configure --prefix="${BDIR}" --enable-openssl --with-openssl-dir="${BDIR}"
  then
    if [ -f config.log ]
    then
      if ! grep -B200 'See `config.log. for more details' config.log
      then
        tail -n 200 config.log
      fi
    fi
    exit 1
  fi
  ${RUN_LOG} ${GMAKE} libsrtp2.a
  ${GMAKE} install
fi

cd ${MYDIR}/../rtpproxy
if [ ! -z "${SAVE_SPACE}" ]
then
  rm -rf ${MYDIR}/../libsrtp
fi

CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" ./configure --enable-static-crypto \
 --enable-librtpproxy --enable-lto --disable-noinst --disable-debug
if ! grep -q '^#define ENABLE_SRTP2 1' src/config.h
then
  grep -C 200 lsrtp2 config.log
  exit 1
fi
for dir in libexecinfo libucl libre external/libelperiodic/src libxxHash modules
do
  make -C ${dir} all
done
make -C src librtpproxy.la
