#!/bin/sh

cd /j/bins/rotocamcast || exit 1

if ! [ -e rotocamcast ] ; then
  make rotocamcast || exit 1
fi

./rotocamcast

