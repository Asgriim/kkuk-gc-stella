find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

option(GC_ENABLE_STELLA_TESTS "Build and test Stella programs using the pinned Docker image" OFF)

set(test_runner "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${test_runner}")

add_library(gc_test_support OBJECT abi/support.cpp ../runtime/runtime.c)
target_link_libraries(gc_test_support PUBLIC stella_gc)

set(abi_scenarios
    graph layouts roots update half_heap
    barrier update_barrier large_init diagnostics
)
foreach(scenario IN LISTS abi_scenarios)
    add_executable(gc_test_${scenario} "abi/${scenario}.cpp")
    target_link_libraries(gc_test_${scenario} PRIVATE gc_test_support)
endforeach()

add_library(gc_test_error_support OBJECT abi/errors/support.cpp)
target_link_libraries(gc_test_error_support PUBLIC gc_test_support)
foreach(scenario IN ITEMS access config descriptors lifecycle oom roots update)
    add_executable(gc_test_error_${scenario} "abi/errors/${scenario}.cpp")
    target_link_libraries(gc_test_error_${scenario} PRIVATE gc_test_error_support gc_test_support)
endforeach()

add_executable(gc_test_c_abi_smoke abi/c_abi_smoke.c)
target_link_libraries(gc_test_c_abi_smoke PRIVATE gc_test_support)

function(register_cases suite)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${test_runner}" --suite "${suite}" --list-names
        OUTPUT_VARIABLE names
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    string(REPLACE "\n" ";" names "${names}")
    foreach(name IN LISTS names)
        add_test(NAME "${name}" COMMAND "${Python3_EXECUTABLE}" "${test_runner}"
            --bin-dir "$<TARGET_FILE_DIR:gc_test_c_abi_smoke>"
            --suite "${suite}" --case "${name}")
        string(REGEX MATCH "^[^/]+" group "${name}")
        set_tests_properties("${name}" PROPERTIES LABELS "${group}" TIMEOUT 25)
    endforeach()
endfunction()

register_cases(abi)

if(GC_ENABLE_STELLA_TESTS)
    find_program(STELLA_DOCKER_EXECUTABLE NAMES docker REQUIRED)
    set(stella_dir "${CMAKE_CURRENT_SOURCE_DIR}/stella")
    file(GLOB sources CONFIGURE_DEPENDS "${stella_dir}/src/*.stella")
    file(GLOB params CONFIGURE_DEPENDS "${stella_dir}/params/*.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${params})
    foreach(source IN LISTS sources)
        get_filename_component(program "${source}" NAME_WE)
        set(generated "${CMAKE_CURRENT_BINARY_DIR}/stella/${program}.c")
        add_custom_command(
            OUTPUT "${generated}"
            COMMAND "${Python3_EXECUTABLE}" "${stella_dir}/compile.py"
                --docker "${STELLA_DOCKER_EXECUTABLE}" "${source}" "${generated}"
            DEPENDS "${source}" "${stella_dir}/compile.py"
            VERBATIM
        )
        add_executable(stella_test_${program} "${generated}")
        target_link_libraries(stella_test_${program} PRIVATE stella_runtime stella_gc)
    endforeach()
    register_cases(stella)
endif()
