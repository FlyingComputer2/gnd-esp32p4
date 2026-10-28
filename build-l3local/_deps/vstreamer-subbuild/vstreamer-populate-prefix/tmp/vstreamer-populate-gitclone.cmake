# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

if(EXISTS "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-subbuild/vstreamer-populate-prefix/src/vstreamer-populate-stamp/vstreamer-populate-gitclone-lastrun.txt" AND EXISTS "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-subbuild/vstreamer-populate-prefix/src/vstreamer-populate-stamp/vstreamer-populate-gitinfo.txt" AND
  "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-subbuild/vstreamer-populate-prefix/src/vstreamer-populate-stamp/vstreamer-populate-gitclone-lastrun.txt" IS_NEWER_THAN "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-subbuild/vstreamer-populate-prefix/src/vstreamer-populate-stamp/vstreamer-populate-gitinfo.txt")
  message(STATUS
    "Avoiding repeated git clone, stamp file is up to date: "
    "'/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-subbuild/vstreamer-populate-prefix/src/vstreamer-populate-stamp/vstreamer-populate-gitclone-lastrun.txt'"
  )
  return()
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E rm -rf "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-src"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to remove directory: '/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-src'")
endif()

# try the clone 3 times in case there is an odd git clone issue
set(error_code 1)
set(number_of_tries 0)
while(error_code AND number_of_tries LESS 3)
  execute_process(
    COMMAND "/usr/bin/git"
            clone --no-checkout --config "advice.detachedHead=false" "https://github.com/FlyingComputer2/vstreamer.git" "vstreamer-src"
    WORKING_DIRECTORY "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps"
    RESULT_VARIABLE error_code
  )
  math(EXPR number_of_tries "${number_of_tries} + 1")
endwhile()
if(number_of_tries GREATER 1)
  message(STATUS "Had to git clone more than once: ${number_of_tries} times.")
endif()
if(error_code)
  message(FATAL_ERROR "Failed to clone repository: 'https://github.com/FlyingComputer2/vstreamer.git'")
endif()

execute_process(
  COMMAND "/usr/bin/git"
          checkout "187c140d41e0f4ef7e8d59e1f585d82bf9d072fa" --
  WORKING_DIRECTORY "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-src"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to checkout tag: '187c140d41e0f4ef7e8d59e1f585d82bf9d072fa'")
endif()

set(init_submodules TRUE)
if(init_submodules)
  execute_process(
    COMMAND "/usr/bin/git" 
            submodule update --recursive --init 
    WORKING_DIRECTORY "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-src"
    RESULT_VARIABLE error_code
  )
endif()
if(error_code)
  message(FATAL_ERROR "Failed to update submodules in: '/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-src'")
endif()

# Complete success, update the script-last-run stamp file:
#
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-subbuild/vstreamer-populate-prefix/src/vstreamer-populate-stamp/vstreamer-populate-gitinfo.txt" "/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-subbuild/vstreamer-populate-prefix/src/vstreamer-populate-stamp/vstreamer-populate-gitclone-lastrun.txt"
  RESULT_VARIABLE error_code
)
if(error_code)
  message(FATAL_ERROR "Failed to copy script-last-run stamp file: '/home/ubuntu/development/gnd-esp32p4-feature-rx-latency/build-l3local/_deps/vstreamer-subbuild/vstreamer-populate-prefix/src/vstreamer-populate-stamp/vstreamer-populate-gitclone-lastrun.txt'")
endif()
