cmake_minimum_required(VERSION 3.29)
project(test)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
        CONFIGURE_DEPENDS
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
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "CalendarApp")

if (WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(${PROJECT_NAME} PRIVATE /ENTRY:mainCRTStartup)
endif()

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
        _WIN32_WINNT=0x0601
)

# ----------------------------
# Libraries
# ----------------------------

target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        ${wxWidgets_LIBRARIES}
)

if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
endif()

# ----------------------------
# Distribution install layout
# ----------------------------

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION .
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        DESTINATION .
)

if (WIN32)
    install(CODE [[
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES "$<TARGET_FILE:test>"
            RESOLVED_DEPENDENCIES_VAR runtime_deps
            UNRESOLVED_DEPENDENCIES_VAR unresolved_deps
            PRE_EXCLUDE_REGEXES
                "api-ms-.*"
                "ext-ms-.*"
            POST_EXCLUDE_REGEXES
                ".*[Ww]indows[\\/].*"
                ".*[Ss]ystem32[\\/].*"
        )

        if(runtime_deps)
            file(INSTALL
                DESTINATION "${CMAKE_INSTALL_PREFIX}"
                TYPE FILE
                FILES ${runtime_deps}
            )
        endif()

        if(unresolved_deps)
            message(WARNING "Unresolved runtime dependencies: ${unresolved_deps}")
        endif()
    ]])
endif()
