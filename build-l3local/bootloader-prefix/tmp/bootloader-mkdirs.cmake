# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/ubuntu/esp/esp-idf-v5.5.5/components/bootloader/subproject"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/bootloader"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/bootloader-prefix"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/bootloader-prefix/tmp"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/bootloader-prefix/src/bootloader-stamp"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/bootloader-prefix/src"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
