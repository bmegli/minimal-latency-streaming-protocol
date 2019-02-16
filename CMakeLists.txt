cmake_minimum_required(VERSION 3.0)

project(
    mlsp
)

add_library(mlsp SHARED mlsp.c)
install(TARGETS mlsp DESTINATION lib)
install(FILES mlsp.h DESTINATION include)

#add_executable(mlsp-example examples/mlsp_example.c)
#target_link_libraries(mlsp-example mlsp)

