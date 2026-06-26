# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/neet821/.espressif/v6.0.1/esp-idf/components/bootloader/subproject"
  "/home/neet821/Desktop/wuxiankeneng/smart_home_controller/build/bootloader"
  "/home/neet821/Desktop/wuxiankeneng/smart_home_controller/build/bootloader-prefix"
  "/home/neet821/Desktop/wuxiankeneng/smart_home_controller/build/bootloader-prefix/tmp"
  "/home/neet821/Desktop/wuxiankeneng/smart_home_controller/build/bootloader-prefix/src/bootloader-stamp"
  "/home/neet821/Desktop/wuxiankeneng/smart_home_controller/build/bootloader-prefix/src"
  "/home/neet821/Desktop/wuxiankeneng/smart_home_controller/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/neet821/Desktop/wuxiankeneng/smart_home_controller/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/neet821/Desktop/wuxiankeneng/smart_home_controller/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
