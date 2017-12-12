#!/bin/bash -eux

#/usr/bin/h5c++ -std=c++14 -fmessage-length=0 -O3 example.cpp -o example.exe

/usr/bin/h5c++ -std=c++14 -fmessage-length=0 -Itclap-1.2.1/include -O3 write.cpp fastboard.cpp sgf.cpp core/*.cpp -o write.exe
