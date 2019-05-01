#!/bin/sh

cd /j/bins/rotocamcast || exit 1

if ! [ -e rotocamcast ] ; then
  make rotocamcast
fi

./rotocamcast

