cmake_minimum_required(VERSION 3.29)
project(test)

set(CMAKE_CXX_STANDARD 20)

# ----------------------------
# External libraries
# ----------------------------

set(BOOST_ROOT "C:/dev/cppLibs/boost_1_89_0")
find_package(OpenSSL REQUIRED)
find_package(wxWidgets CONFIG REQUIRED COMPONENTS core base)

# ----------------------------
# SQLiteCpp
# ----------------------------

set(SQLITECPP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SQLiteCpp/include)
set(SQLITECPP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SQLiteCpp/src)
set(SQLITECPP_SQLITE3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SQLiteCpp/sqlite3)

file(GLOB_RECURSE SQLITECPP_SOURCES
        "${SQLITECPP_SRC_DIR}/*.cpp"
        "${SQLITECPP_SQLITE3_DIR}/*.c"
)

# ----------------------------
# Project sources
# ----------------------------

set(PROJECT_SOURCES
        main.cpp
)

file(GLOB_RECURSE PROJECT_SOURCES_EXTRA
        "src/*.cpp"
)

set(ALL_SOURCES
        ${PROJECT_SOURCES}
        ${PROJECT_SOURCES_EXTRA}
        ${SQLITECPP_SOURCES}
)

# ----------------------------
# Executable
# ----------------------------

add_executable(${PROJECT_NAME} ${ALL_SOURCES})

# ----------------------------
# Include directories (IMPORTANT FIX)
# ----------------------------

target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SQLITECPP_INCLUDE_DIR}
        ${SQLITECPP_SQLITE3_DIR}
        ${BOOST_ROOT}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
        APP_DB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/calendar_app.db"
)

# ----------------------------
# Libraries
# ----------------------------

target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        ws2_32
        ${wxWidgets_LIBRARIES}
)
