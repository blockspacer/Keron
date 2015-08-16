#!/bin/sh
PREFIX=$1
PREMAKE_VERSION=5.0.0.alpha4-linux
PREMAKE_URL=https://github.com/premake/premake-core/releases/download/v5.0.0.alpha4/premake-${PREMAKE_VERSION}.tar.gz
CMAKE_VERSION=3.3.1-Linux-x86_64
CMAKE_URL=http://www.cmake.org/files/v3.3/cmake-${CMAKE_VERSION}.tar.gz

[ ! -d $PREFIX/tools ] && mkdir -p $PREFIX/tools/bin

# Premake
wget -O /tmp/premake-dev-linux.tar.gz $PREMAKE_URL
ls -l /tmp/premake-dev-linux.tar.gz
file /tmp/premake-dev-linux.tar.gz
tar -C $PREFIX/tools/bin -xzf /tmp/premake-dev-linux.tar.gz

# CMake
wget -O /tmp/cmake-linux.tar.gz $CMAKE_URL
ls -l /tmp/cmake-linux.tar.gz
file /tmp/cmake-linux.tar.gz
tar -C $PREFIX/tools -xzf /tmp/cmake-linux.tar.gz
ln -s $PWD/cmake-$CMAKE_VERSION/bin/cmake $PREFIX/tools/bin
ln -s $PWD/cmake-$CMAKE_VERSION/bin/cpack $PREFIX/tools/bin
ln -s $PWD/cmake-$CMAKE_VERSION/bin/ctest $PREFIX/tools/bin

# KSP runtime archive.
wget --no-check-certificate -q -O /tmp/ksp-runtime-linux.7z $ARCHIVE_URL
7z -h
7z x -p$ARCHIVE_PWD /tmp/ksp-runtime-linux.7z

touch .valid
