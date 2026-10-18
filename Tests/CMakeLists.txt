add_executable(DimensionTests Source/EngineTests.cpp)
target_link_libraries(DimensionTests PRIVATE DimensionEngine)
target_compile_definitions(DimensionTests PRIVATE DIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME EngineTests COMMAND DimensionTests)
