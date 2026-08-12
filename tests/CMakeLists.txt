add_library(bv_testtree STATIC
  TestTree.cpp
)
target_include_directories(bv_testtree PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ../src)
target_link_libraries(bv_testtree PUBLIC bv_core)

add_executable(bv_tests test_main.cpp)
target_link_libraries(bv_tests PRIVATE bv_testtree)

if(BUILD_TESTGEN)
  add_executable(bv_testgen ../tools/testgen.cpp)
  target_link_libraries(bv_testgen PRIVATE bv_testtree shell32)
  add_executable(bv_mftbench ../tools/mftbench.cpp)
  target_link_libraries(bv_mftbench PRIVATE bv_testtree shell32)
  add_executable(bv_mftprobe ../tools/mftprobe.cpp)
  target_link_libraries(bv_mftprobe PRIVATE shell32)
  add_executable(bv_mftdiag ../tools/mftdiag.cpp)
  target_link_libraries(bv_mftdiag PRIVATE bv_testtree shell32)
endif()

add_test(NAME unit COMMAND bv_tests)
