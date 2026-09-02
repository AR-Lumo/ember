# ember::codegen - one pipeline stage, per the project layout in section 2.
#
# This is the only stage that needs LLVM. When LLVM is absent the library
# still builds, but `is_available()` reports false and the driver refuses
# `ember build` with a clear message instead of failing to link.

add_library(ember_codegen STATIC src/codegen.cpp)
add_library(ember::codegen ALIAS ember_codegen)

target_include_directories(ember_codegen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ember_codegen PUBLIC cxx_std_20)

target_link_libraries(ember_codegen PUBLIC ember::ast ember::typeck)

if(EMBER_HAVE_LLVM)
    target_compile_definitions(ember_codegen PUBLIC EMBER_HAVE_LLVM=1)
    target_include_directories(ember_codegen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    separate_arguments(EMBER_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_definitions(ember_codegen PRIVATE ${EMBER_LLVM_DEFINITIONS})

    # The components codegen actually uses: IR construction, the host
    # target's object emission, and the new pass manager.
    llvm_map_components_to_libnames(EMBER_LLVM_LIBS
        core
        support
        irreader
        passes
        analysis
        target
        nativecodegen
    )
    target_link_libraries(ember_codegen PRIVATE ${EMBER_LLVM_LIBS})

    # LLVM is built without RTTI by default; a translation unit that
    # inherits from its classes has to match or the vtables disagree.
    if(NOT LLVM_ENABLE_RTTI AND NOT MSVC)
        target_compile_options(ember_codegen PRIVATE -fno-rtti)
    endif()
else()
    target_compile_definitions(ember_codegen PUBLIC EMBER_HAVE_LLVM=0)
endif()
