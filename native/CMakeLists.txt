cmake_minimum_required(VERSION 3.18)
project(mmdbg CXX)

# 两个独立模块，各编各的 so，互不依赖
#   libmmdbg_model.so   Unity / il2cpp 逻辑（靠 Java 的 tick 进主线程）
#   libmmdbg_log.so     日志模块（自己起线程，不需要 tick）
add_library(mmdbg_model SHARED model.cpp)
target_compile_options(mmdbg_model PRIVATE -O2 -std=c++17)
find_library(log-lib log)
target_link_libraries(mmdbg_model ${log-lib} dl)

add_library(mmdbg_log SHARED log.cpp)
target_compile_options(mmdbg_log PRIVATE -O2 -std=c++17)
target_link_libraries(mmdbg_log ${log-lib})

# 不要 -fvisibility=hidden，JNI 符号要导出
set_target_properties(mmdbg_model PROPERTIES OUTPUT_NAME mmdbg_model)
set_target_properties(mmdbg_log   PROPERTIES OUTPUT_NAME mmdbg_log)
