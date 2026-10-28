# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/winject_sdio_proxy-src"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/winject_sdio_proxy-build"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/winject_sdio_proxy-subbuild/winject_sdio_proxy-populate-prefix"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/winject_sdio_proxy-subbuild/winject_sdio_proxy-populate-prefix/tmp"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/winject_sdio_proxy-subbuild/winject_sdio_proxy-populate-prefix/src/winject_sdio_proxy-populate-stamp"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/winject_sdio_proxy-subbuild/winject_sdio_proxy-populate-prefix/src"
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/winject_sdio_proxy-subbuild/winject_sdio_proxy-populate-prefix/src/winject_sdio_proxy-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/winject_sdio_proxy-subbuild/winject_sdio_proxy-populate-prefix/src/winject_sdio_proxy-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/winject_sdio_proxy-subbuild/winject_sdio_proxy-populate-prefix/src/winject_sdio_proxy-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
