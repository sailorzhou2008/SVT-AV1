#!/bin/bash
#
pwd
./Build/linux/ci_build.sh clean
./Build/linux/ci_build.sh release
ls