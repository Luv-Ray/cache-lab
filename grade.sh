#!/bin/bash

BUILD_PATH="build/X86/gem5.opt"
DIRECT_CONFIG_PATH="configs/labs/direct_cache.py"
FULLY_ASSOC_CONFIG_PATH="configs/labs/fully_assoc_cache.py"
SET_ASSOC_CONFIG_PATH="configs/labs/set_assoc_cache.py"

echo_red() {
  echo -e "\033[31m$1\033[0m"
}

echo_green() {
  echo -e "\033[32m$1\033[0m"
}

run_command() {
  echo_green "running command: $1"
  eval "$1"
  if [ $? -ne 0 ]; then
    echo_red "Execution failed, please check the error message!"
    exit 1
  fi
}

# build gem5.opt
echo_green "building $BUILD_PATH..."
scons build/X86/gem5.opt -j $(nproc)
if [ $? -ne 0 ]; then
  echo_red "build failed!"
  exit 1
else
  echo_green "build success!"
fi

# run simulation
direct_ticks="0"
fully_assoc_ticks="0"
fully_assoc_ticks_optimized="0"
set_assoc_ticks="0"

# simulate direct mapped cache
run_command "$BUILD_PATH $DIRECT_CONFIG_PATH"
direct_ticks=$(grep "simTicks" "m5out/stats.txt" | awk '{print $2}')
echo_green "sim_ticks: $direct_ticks"

# simulate fully associative cache
run_command "$BUILD_PATH $FULLY_ASSOC_CONFIG_PATH --algorithm=\"random\""
fully_assoc_ticks=$(grep "simTicks" "m5out/stats.txt" | awk '{print $2}')
echo_green "sim_ticks: $fully_assoc_ticks"


# simulate fully associative cache with optimized algorithm
run_command "$BUILD_PATH $FULLY_ASSOC_CONFIG_PATH --algorithm=\"optimized\""
fully_assoc_ticks_optimized=$(grep "simTicks" "m5out/stats.txt" | awk '{print $2}')
echo_green "sim_ticks: $fully_assoc_ticks_optimized"

# simulate set associative cache
run_command "$BUILD_PATH $SET_ASSOC_CONFIG_PATH"
set_assoc_ticks=$(grep "simTicks" "m5out/stats.txt" | awk '{print $2}')
echo_green "sim_ticks: $set_assoc_ticks"

# run tests
# test1: implement set associative cache
if [ "$direct_ticks" -lt "$set_assoc_ticks" ]; then
  echo_red "Set associative cache should run less ticks than direct mapped cache!"
else
  echo_green "test1 success"
fi

# test2: replacement algorithm
if [ "$fully_assoc_ticks_optimized" -gt $(($fully_assoc_ticks * 8 / 10)) ]; then
  echo_red "Fully associative cache's replacement algorithm didn't improve!"
else
  echo_green "test2 success"
fi
