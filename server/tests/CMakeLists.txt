add_executable(fastbid_tests
    test_object_pool.cpp
    test_spsc_queue.cpp
    test_matching_engine.cpp
    test_reference.cpp
)

target_link_libraries(fastbid_tests PRIVATE
    fastbid_core
    GTest::gtest_main
    pthread
)

target_compile_features(fastbid_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(fastbid_tests)
