set(sources
    qc_counters.h
)

if(ANDROID)
    list(APPEND sources
        qc_counters.cpp
    )
else()
    list(APPEND sources
        qc_counters_stub.cpp
    )
endif()

set_property(SOURCE ${sources}
    PROPERTY COMPILE_FLAGS "-Wno-unknown-warning-option -Wno-range-loop-construct")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_property(SOURCE ${sources} APPEND_STRING PROPERTY COMPILE_FLAGS " -Wno-shadow -Wno-shorten-64-to-32")
endif()

add_library(rdoc_qc OBJECT ${sources})

set(include_dirs
    ${RDOC_INCLUDES}
)
target_include_directories(rdoc_qc ${include_dirs})
target_compile_definitions(rdoc_qc ${RDOC_DEFINITIONS})
