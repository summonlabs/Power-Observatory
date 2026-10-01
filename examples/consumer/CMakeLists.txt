cmake_minimum_required(VERSION 3.21)

project(power-observatory-consumer LANGUAGES CXX)

# A downstream consumer of the installed Power Observatory package. It is
# deliberately not part of the Power Observatory build tree: it exists to
# prove that an installed artifact is usable by an independent project.
find_package(PowerObservatory 1.0 REQUIRED)

add_executable(po-consumer consumer_main.cpp)
target_link_libraries(po-consumer PRIVATE PowerObservatory::power_observatory)
target_compile_features(po-consumer PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(po-consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(po-consumer PRIVATE -Wall -Wextra -Werror)
endif()
