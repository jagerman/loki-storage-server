#!/bin/bash

bash -i

set -e
set -x

#curl -L https://dl.bintray.com/boostorg/release/1.74.0/source/boost_1_74_0.tar.bz2 | tar xj
#curl -L https://deb.imaginary.stream/boost_1_72_0.tar.bz2 | tar xj

curl -L https://builds.lokinet.dev/deps/boost_1_74_0.tar.bz2 | tar xj

cd boost_1_74_0

export CC=clang-8 CXX=clang++-8

echo "using clang : : clang++-8 : <cxxflags>-stdlib=libc++ <ranlib>llvm-ranlib-8 <archiver>llvm-ar-8 ;" >user-config.jam

./bootstrap.sh --without-icu --with-toolset=clang --with-libraries=filesystem,program_options,system,thread

./b2 -d0 -a --prefix=${PWD}/../boost link=static runtime-link=static variant=release optimization=speed \
        cxxflags=-fPIC cxxstd=14 visibility=global \
        lto=on \
        --disable-icu \
        --user-config=${PWD}/user-config.jam \
        install


