if(SOLIDAR_ENABLE_SANITIZERS)
  if(MSVC)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
      message(FATAL_ERROR
              "The supported MSVC AddressSanitizer profile requires x64")
    endif()
    get_filename_component(SOLIDAR_SANITIZER_RUNTIME_DIRECTORY
                           "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(NOT EXISTS
       "${SOLIDAR_SANITIZER_RUNTIME_DIRECTORY}/clang_rt.asan_dynamic-x86_64.dll")
      message(FATAL_ERROR
              "The MSVC AddressSanitizer runtime is not installed next to cl.exe")
    endif()
    add_compile_options(/fsanitize=address /Zi /EHsc)
    add_link_options(/fsanitize=address /INCREMENTAL:NO)
    set(SOLIDAR_SANITIZER_TEST_ENVIRONMENT
        "ASAN_OPTIONS=set:halt_on_error=1:detect_leaks=0")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=address,undefined
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    set(SOLIDAR_SANITIZER_TEST_ENVIRONMENT
        "ASAN_OPTIONS=set:halt_on_error=1:detect_leaks=0"
        "UBSAN_OPTIONS=set:halt_on_error=1:print_stacktrace=1")
  else()
    message(FATAL_ERROR
            "SOLIDAR_ENABLE_SANITIZERS supports MSVC, Clang and GCC only")
  endif()
endif()

if(SOLIDAR_ENABLE_CLANG_TIDY)
  find_program(SOLIDAR_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set(CMAKE_CXX_CLANG_TIDY
      "${SOLIDAR_CLANG_TIDY_EXECUTABLE};--quiet"
      CACHE STRING "clang-tidy command used by CMake" FORCE)
endif()
