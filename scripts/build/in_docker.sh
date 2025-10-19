#!/bin/sh

set -e
set -x

ENV="`${SET_ENV}`"
IFS=$'\n' && set -- ${ENV} && IFS=''
env "${@}" ${PYTHON_CMD} -m build --wheel
