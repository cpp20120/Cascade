cmake_minimum_required(VERSION 3.26)
project(Cascade
  VERSION 1.0.0
  DESCRIPTION "High-performance C++20 task runtime with arena allocation and work-stealing"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CASCADE_VERSION ${PROJECT_VERSION})

# Configuration options
option(CASCADE_BUILD_SHARED "Build Cascade as shared library" ON)
option(CASCADE_BUILD_STATIC "Build Cascade as static library" OFF)
option(CASCADE_BUILD_EXAMPLES "Build examples" OFF)
option(CASCADE_BUILD_BENCH "Build benchmarks" OFF)
option(CASCADE_BUILD_TESTS "Build tests" OFF)
option(CASCADE_INSTALL "Generate install target" OFF)

# Create interface library for public headers
add_library(cascade_headers INTERFACE)
target_include_directories(cascade_headers INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(cascade_headers INTERFACE cxx_std_23)


set(CASCADE_MEMORY_SOURCES
    src/memory/arena.cpp
    src/memory/global_reservoir.cpp
    src/memory/allocator.cpp
)

set(CASCADE_QUEUES_SOURCES
    src/queues/work_stealing_queue.cpp
    src/queues/bounded_mpmc.cpp
    src/queues/mpsc_stack.cpp
)

set(CASCADE_SCHEDULING_SOURCES
    src/scheduling/worker.cpp
    src/scheduling/notification_manager.cpp
    src/scheduling/quarry.cpp
    src/scheduling/shutdown.cpp
)

set(CASCADE_DAG_SOURCES
    src/dag/task_graph.cpp
    src/dag/dependency_manager.cpp
    src/dag/ready_set.cpp
)

set(CASCADE_API_SOURCES
    src/api/pool.cpp
    src/api/scope.cpp
    src/api/parallel_for.cpp
    src/api/future.cpp
)

# Combine all sources
set(CASCADE_ALL_SOURCES
    ${CASCADE_CORE_SOURCES}
    ${CASCADE_MEMORY_SOURCES}
    ${CASCADE_QUEUES_SOURCES}
    ${CASCADE_SCHEDULING_SOURCES}
    ${CASCADE_DAG_SOURCES}
    ${CASCADE_API_SOURCES}
)

# Object library for all sources
add_library(cascade_sources OBJECT ${CASCADE_ALL_SOURCES})

# Apply common properties to source objects
target_compile_features(cascade_sources PUBLIC cxx_std_23)
target_include_directories(cascade_sources PUBLIC include)

if(MSVC)
    target_compile_options(cascade_sources PRIVATE  /permissive-)
else()
    target_compile_options(cascade_sources PRIVATE -Wall -Wextra -pedantic -Werror)
endif()

# Shared library
if(CASCADE_BUILD_SHARED)
    add_library(Cascade SHARED)
    target_sources(Cascade PRIVATE $<TARGET_OBJECTS:cascade_sources>)
    
    # Link headers interface
    target_link_libraries(Cascade PUBLIC cascade_headers)
    
    set_target_properties(Cascade PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
        POSITION_INDEPENDENT_CODE ON
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        OUTPUT_NAME "Cascade"
    )
endif()

# Static library  
if(CASCADE_BUILD_STATIC)
    add_library(CascadeStatic STATIC)
    target_sources(CascadeStatic PRIVATE $<TARGET_OBJECTS:cascade_sources>)
    target_link_libraries(CascadeStatic PUBLIC cascade_headers)
    set_target_properties(CascadeStatic PROPERTIES OUTPUT_NAME "Cascade")
endif()


# Examples
#if(CASCADE_BUILD_EXAMPLES)
#    add_subdirectory(examples)
#endif()

# Benchmarks
#if(CASCADE_BUILD_BENCH)
#    add_subdirectory(bench)
    
    # Find TBB for benchmarks if available
#    find_package(TBB QUIET)
#    if(TBB_FOUND)
#        message(STATUS "TBB found: benchmarks will include TBB comparisons")
#    else()
#        message(STATUS "TBB not found: benchmarks will run without TBB comparisons")
#    endif()
#endif()

# Tests
if(CASCADE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# Installation
#if(CASCADE_INSTALL)
#    include(GNUInstallDirs)
#    
    # Install headers recursively maintaining structure
#    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    
    # Install libraries
#    if(CASCADE_BUILD_SHARED)
#        install(TARGETS Cascade
#            EXPORT CascadeTargets
#            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
#            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
#            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
#        )
#    endif()
    
#    if(CASCADE_BUILD_STATIC)
#        install(TARGETS CascadeStatic
#            EXPORT CascadeTargets
#            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
#        )
#    endif()
    
    # Install export targets
#    install(EXPORT CascadeTargets
#        FILE CascadeConfig.cmake
#        NAMESPACE Cascade::
#        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cascade
#    )
    
    # Generate and install package config
#    include(CMakePackageConfigHelpers)
#    configure_package_config_file(
#        cmake/CascadeConfig.cmake.in
#        ${CMAKE_CURRENT_BINARY_DIR}/CascadeConfig.cmake
#        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cascade
#    )
    
#    write_basic_package_version_file(
#        ${CMAKE_CURRENT_BINARY_DIR}/CascadeConfigVersion.cmake
#        VERSION ${PROJECT_VERSION}
#        COMPATIBILITY SameMajorVersion
#    )
    
#    install(FILES
#        ${CMAKE_CURRENT_BINARY_DIR}/CascadeConfig.cmake
#        ${CMAKE_CURRENT_BINARY_DIR}/CascadeConfigVersion.cmake
#        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cascade
#    )
#endif()

# Print configuration summary
message(STATUS "")
message(STATUS "Cascade Configuration Summary:")
message(STATUS "  Version: ${CASCADE_VERSION}")
message(STATUS "  Build shared: ${CASCADE_BUILD_SHARED}")
message(STATUS "  Build static: ${CASCADE_BUILD_STATIC}")
message(STATUS "  Build examples: ${CASCADE_BUILD_EXAMPLES}")
message(STATUS "  Build benchmarks: ${CASCADE_BUILD_BENCH}")
message(STATUS "  Build tests: ${CASCADE_BUILD_TESTS}")
#message(STATUS "  Install: ${CASCADE_INSTALL}")
message(STATUS "")