cmake_minimum_required(VERSION 3.29)
project(test)

# Nastavenie C++ štandardu
set(CMAKE_CXX_STANDARD 20)

#Cesta k Boost
set(BOOST_ROOT "C:/dev/cppLibs/boost_1_89_0")
include_directories(${BOOST_ROOT})

find_package(OpenSSL REQUIRED)

# ---- SQLiteCpp ----
set(SQLITECPP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SQLiteCpp/include)
set(SQLITECPP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SQLiteCpp/src)
set(SQLITECPP_SQLITE3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SQLiteCpp/sqlite3)

file(GLOB_RECURSE SQLITECPP_SOURCES
        "${SQLITECPP_SRC_DIR}/*.cpp"
        "${SQLITECPP_SQLITE3_DIR}/*.c"
)

# ---- Project sources ----
set(PROJECT_SOURCES
        main.cpp
)

# ---- Utils ----
file(GLOB_RECURSE UTILS_SOURCES
        "include/utils/*.h"
        "src/utils/*.cpp"
)

# ---- OAuth ----
file(GLOB_RECURSE OAUTH_SOURCES
        "include/oauth/*.h"
        "src/oauth/*.cpp"
)

# ---- Events ----
file(GLOB_RECURSE EVENTS_SOURCES
        "include/events/*.h"
        "src/events/*.cpp"
)

# Všetky zdrojáky spolu
set(ALL_SOURCES
        ${PROJECT_SOURCES}
        ${SQLITECPP_SOURCES}
        ${UTILS_SOURCES}
        ${OAUTH_SOURCES}
        ${EVENTS_SOURCES}
)

# ---- Include directories ----
set(PROJECT_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SQLITECPP_INCLUDE_DIR}
        ${SQLITECPP_SQLITE3_DIR}
        include/utils
        include/oauth
        include/events
)

# Vytvorenie spustiteľného súboru
add_executable(${PROJECT_NAME} ${ALL_SOURCES})

# Pridanie include directories
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_INCLUDE_DIRS})

# Prepojenie s knižnicami
target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        ws2_32
)
