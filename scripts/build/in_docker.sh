#!/bin/sh

set -e
set -x

ENV="`${SET_ENV}`"
if [ "${?}" -ne 0 ]
then
  exit "${?}"
fi
IFS=$'\n' && set -- ${ENV} && IFS=''
env "${@}" ${PYTHON_CMD} -m build --wheel
