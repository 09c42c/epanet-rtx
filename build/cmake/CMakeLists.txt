#  ________  _______    ______   __    __  ________  ________
# |        \|       \  /      \ |  \  |  \|        \|        \
# | $$$$$$$$| $$$$$$$\|  $$$$$$\| $$\ | $$| $$$$$$$$ \$$$$$$$$
# | $$__    | $$__/ $$| $$__| $$| $$$\| $$| $$__       | $$
# | $$  \   | $$    $$| $$    $$| $$$$\ $$| $$  \      | $$
# | $$$$$   | $$$$$$$ | $$$$$$$$| $$\$$ $$| $$$$$      | $$
# | $$_____ | $$      | $$  | $$| $$ \$$$$| $$_____    | $$
# | $$     \| $$      | $$  | $$| $$  \$$$| $$     \   | $$
#  \$$$$$$$$ \$$       \$$   \$$ \$$   \$$ \$$$$$$$$    \$$
#
#
#
#              _______  ________  __    __
#             |       \|        \|  \  |  \
#             | $$$$$$$\\$$$$$$$$| $$  | $$
#             | $$__| $$  | $$    \$$\/  $$
#             | $$    $$  | $$     >$$  $$
#             | $$$$$$$\  | $$    /  $$$$\
#             | $$  | $$  | $$   |  $$ \$$\
#             | $$  | $$  | $$   | $$  | $$
#              \$$   \$$   \$$    \$$   \$$
#
#


cmake_minimum_required (VERSION 3.10)

project (EPANET-RTX)
SET (CMAKE_C_FLAGS "-std=c99")
SET (CMAKE_CXX_FLAGS "-Wall")
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

FIND_PACKAGE (Boost 1.74.0 REQUIRED)
FIND_PACKAGE (cpprestsdk 2.10.18 REQUIRED)
FIND_PACKAGE (epanet2 REQUIRED)
FIND_PACKAGE (nlohmann_json 3.9 REQUIRED)
FIND_PACKAGE (oatpp REQUIRED)
FIND_PACKAGE (oatpp-openssl REQUIRED)
FIND_PACKAGE (CURL REQUIRED)

set(CONAN_DISABLE_CHECK_COMPILER True)
include(../../deps/conanbuildinfo.cmake)
conan_basic_setup()

# the rtx library
include_directories(
  ${CONAN_INCLUDE_DIRS}
        ../../src
        /usr/local/include
        /usr/local/include/epanet2
        /usr/local/include/iODBC
        /usr/include
        /usr/include/iodbc
        /usr/local/include/oatpp-1.3.0/oatpp
        /usr/local/include/oatpp-1.3.0/oatpp-openssl
        ${Boost_LIBRARIES}
)

link_directories(
        /usr/lib
        /usr/local/lib
        /usr/local/lib/oatpp-1.3.0
)

add_library(epanet-rtx SHARED
../../src/AggregatorTimeSeries.cpp
../../src/BaseStatsTimeSeries.cpp
../../src/BufferPointRecord.cpp
../../src/Clock.cpp
../../src/ConcreteDbRecords.cpp
../../src/ConstantTimeSeries.cpp
../../src/CorrelatorTimeSeries.cpp
../../src/Curve.cpp
../../src/CurveFunction.cpp
../../src/DbPointRecord.cpp
../../src/Dma.cpp
../../src/Element.cpp
../../src/EpanetModel.cpp
../../src/EpanetModelExporter.cpp
../../src/EpanetSyntheticModel.cpp
../../src/FailoverTimeSeries.cpp
../../src/FirstDerivative.cpp
../../src/GainTimeSeries.cpp
../../src/IdentifierUnitsList.cpp
../../src/InfluxAdapter.cpp
../../src/InpTextPattern.cpp
../../src/IntegratorTimeSeries.cpp
../../src/InversionTimeSeries.cpp
../../src/Junction.cpp
../../src/LagTimeSeries.cpp
../../src/Link.cpp
../../src/LogicTimeSeries.cpp
../../src/MathOpsTimeSeries.cpp
../../src/MetaTimeSeries.cpp
../../src/MetricInfo.cpp
../../src/Model.cpp
../../src/MovingAverage.cpp
../../src/MultiplierTimeSeries.cpp
../../src/Node.cpp
../../src/OdbcAdapter.cpp
../../src/OffsetTimeSeries.cpp
../../src/OutlierExclusionTimeSeries.cpp
../../src/PiAdapter.cpp
../../src/Pipe.cpp
../../src/Point.cpp
../../src/PointCollection.cpp
../../src/PointRecord.cpp
../../src/PointRecordTime.cpp
../../src/Pump.cpp
../../src/Reservoir.cpp
../../src/SineTimeSeries.cpp
../../src/SquareWaveTimeSeries.cpp
../../src/SqliteAdapter.cpp
../../src/StatsTimeSeries.cpp
../../src/Tank.cpp
../../src/ThresholdTimeSeries.cpp
../../src/TimeRange.cpp
../../src/TimeSeries.cpp
../../src/TimeSeriesFilter.cpp
../../src/TimeSeriesFilterSecondary.cpp
../../src/TimeSeriesFilterSinglePoint.cpp
../../src/TimeSeriesLowess.cpp
../../src/TimeSeriesQuery.cpp
../../src/TimeSeriesSynthetic.cpp
../../src/Units.cpp
../../src/ValidRangeTimeSeries.cpp
../../src/Valve.cpp
../../src/WhereClause.cpp
)

set_target_properties(
  epanet-rtx PROPERTIES
  CXX_STANDARD 17
)

target_compile_definitions(epanet-rtx PRIVATE MAXFLOAT=3.40282347e+38F)

target_link_libraries(
        epanet-rtx
        epanet2
        curl
        iodbc
        sqlite3
        m
        z
        pthread
        oatpp
        oatpp-openssl
        boost_system
        boost_filesystem
        boost_date_time
        boost_regex
        ${CONAN_LIBS}
        )

install(DIRECTORY ../../src/ DESTINATION include/rtx FILES_MATCHING PATTERN "*.h")
install(TARGETS epanet-rtx 
EXPORT epanet-rtxTargets
LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

include(GNUInstallDirs)
install(EXPORT epanet-rtxTargets
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/epanet-rtx
  FILE epanet-rtxConfig.cmake
)
