# FFT_BENCH shared-engine regression (plan-05 Step 1).
#
# Plain-assert executable (no test framework is wired into this fork) that
# exercises FFTWEngine::configure()'s wisdom-import / planner-mode contract
# in isolation, without a running sdrangelsrv process. Registered with CTest
# so it can run alongside `ctest` in CI or by hand.

add_executable(test_fftwengine
    test_fftwengine.cpp
)

target_include_directories(test_fftwengine PRIVATE
    ${CMAKE_SOURCE_DIR}/sdrbase
    ${FFTW3F_INCLUDE_DIRS}
)

target_link_libraries(test_fftwengine PRIVATE
    sdrbase
    ${FFTW3F_LIBRARIES}
    Qt::Core
)

add_test(
    NAME fftwengine_wisdom_contract
    COMMAND test_fftwengine
)
set_tests_properties(fftwengine_wisdom_contract PROPERTIES
    TIMEOUT 60
)
