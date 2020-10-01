#!/bin/bash

bash -i

set -e
set -x

#curl -L https://dl.bintray.com/boostorg/release/1.74.0/source/boost_1_74_0.tar.bz2 | tar xj
#curl -L https://deb.imaginary.stream/boost_1_72_0.tar.bz2 | tar xj

curl -L https://builds.lokinet.dev/deps/boost_1_74_0.tar.bz2 | tar xj

cd boost_1_74_0

export CC=clang-8 CXX=clang++-8

./bootstrap.sh

./b2 -a --prefix=${PWD}/../boost link=static variant=release install \
        --with-program_options \
        --with-filesystem \
        --with-system \
        --with-thread \
        --with-log \
        --with-test \
        --lto


