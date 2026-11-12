add_executable(DimensionTests Source/EngineTests.cpp)
target_link_libraries(DimensionTests PRIVATE DimensionEngine)
target_compile_definitions(DimensionTests PRIVATE DIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}" DIM_UFBX_DATA="${ufbx_SOURCE_DIR}/data")
if(MSVC)
    target_compile_options(DimensionTests PRIVATE /bigobj) # one large file of tests
endif()
add_test(NAME EngineTests COMMAND DimensionTests)
