cmake_minimum_required (VERSION 3.10)

project (EPANET-RTX)
SET (CMAKE_C_FLAGS "-std=c99")
SET (CMAKE_CXX_FLAGS "-Wall -std=c++17")
SET (CMAKE_POSITION_INDEPENDENT_CODE ON)
add_definitions(-DRTX_NO_MYSQL)


message("prefix: ${CMAKE_FIND_LIBRARY_PREFIXES}")
message("suffix: ${CMAKE_FIND_LIBRARY_SUFFIXES}")

SET (EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
SET (LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)

IF(APPLE)
   FIND_LIBRARY(SECURITY_FRAMEWORK Security)
   FIND_LIBRARY(CORE_FRAMEWORK CoreFoundation)
   SET(EXTRA_LIBS ${SECURITY_FRAMEWORK} ${CORE_FRAMEWORK})
ENDIF (APPLE)

FIND_PACKAGE( ZLIB REQUIRED )
IF (ZLIB_FOUND)
        MESSAGE( STATUS "CMAKE found zlib")
ENDIF(ZLIB_FOUND)

FIND_PACKAGE (epanet-rtx REQUIRED)
FIND_PACKAGE (oatpp REQUIRED)
FIND_PACKAGE (oatpp-openssl REQUIRED)

include_directories(
  /usr/local/include
  /usr/local/include/iODBC
  /usr/include/iodbc
  /usr/include
  /usr/local/include/rtx
)

# the AutoRunner class
add_library(auto_runner STATIC AutoRunner.cpp)

# the link service executable
link_directories(
/usr/local/lib
)

add_executable(link-server LinkService.cpp LinkJsonSerialization.cpp main.cpp)

target_link_libraries(link-server
PUBLIC
z auto_runner epanet-rtx 
boost_iostreams boost_thread boost_program_options boost_chrono
pthread ssl cpprest crypto ${EXTRA_LIBS})

install(TARGETS link-server DESTINATION bin)
