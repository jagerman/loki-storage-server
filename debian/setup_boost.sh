#!/bin/bash

bash -i

set -e
set -x

#curl -L https://dl.bintray.com/boostorg/release/1.74.0/source/boost_1_74_0.tar.bz2 | tar xj
#curl -L https://deb.imaginary.stream/boost_1_72_0.tar.bz2 | tar xj

curl -L https://builds.lokinet.dev/deps/boost_1_74_0.tar.bz2 | tar xj

cd boost_1_74_0

export CC=clang-8 CXX=clang++-8

echo "using clang : : clang++-8 : <ranlib>llvm-ranlib-8 <archiver>llvm-ar-8 ;" >user-config.jam

./bootstrap.sh --without-icu --with-toolset=clang --with-libraries=filesystem,program_options,system,thread

./b2 -a --prefix=${PWD}/../boost link=static variant=release install \
        --with-program_options \
        --with-filesystem \
        --with-system \
        --with-thread \
        --with-log \
        --with-test \
        --lto \
        --user-config=./user-config.jam


