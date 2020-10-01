#!/bin/bash
set -ex

curl -L https://www.openssl.org/source/openssl-1.1.1h.tar.gz | tar xz

cd openssl-1.1.1h
./config --prefix=$PWD/../libssl no-shared no-tests no-idea no-mdc2 no-rc5 no-zlib no-ssl3 no-ssl3-method enable-rfc3779 enable-cms enable-ec_nistp_64_gcc_128
make
make install_sw
