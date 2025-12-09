#! /bin/bash

cd build
cmake ..
make install
cd ..
scp -r install/ root@192.168.1.158:/root