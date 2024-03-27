cmake_minimum_required(VERSION 3.20)

project(tcp-example)

include(FetchContent)

set(BOOST_INCLUDE_LIBRARIES thread filesystem system asio)
set(BOOST_ENABLE_CMAKE ON)
FetchContent_Declare(
  Boost
  URL https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.xz
  URL_MD5 893b5203b862eb9bbd08553e24ff146a
)
FetchContent_MakeAvailable(Boost)


FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.x
)

FetchContent_GetProperties(spdlog)
if (NOT spdlog_POPULATED)
    FetchContent_Populate(spdlog)
    add_subdirectory(${spdlog_SOURCE_DIR} ${spdlog_BINARY_DIR})
endif ()

# Build
set(CMAKE_CXX_STANDARD 23)
add_compile_options(
	-g
	-O3
	-Wall
	-Wextra
	-Werror
	-pedantic-errors
	-Wno-missing-field-initializers
)

add_executable(tcp-server src/server.cpp)
target_include_directories(tcp-server BEFORE PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tcp-server PUBLIC Boost::asio spdlog::spdlog)
