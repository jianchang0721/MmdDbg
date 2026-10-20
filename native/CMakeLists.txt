cmake_minimum_required(VERSION 3.18)
project(mmdbg CXX)

add_library(mmdbg SHARED libmmdbg.cpp)

# 注意：不要加 -fvisibility=hidden，JNI 函数必须导出给 dlsym/System.load 用
target_compile_options(mmdbg PRIVATE -O2 -std=c++17)

find_library(log-lib log)
target_link_libraries(mmdbg ${log-lib} dl)
