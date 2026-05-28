cmake_minimum_required(VERSION 3.29)
project(test)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------
# External libraries
# ----------------------------

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(wxWidgets CONFIG QUIET COMPONENTS core base)
if (NOT wxWidgets_FOUND)
    find_package(wxWidgets REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})
endif()

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
elseif (APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
            INSTALL_RPATH "@executable_path"
            BUILD_WITH_INSTALL_RPATH FALSE
    )
elseif (UNIX)
    set_target_properties(${PROJECT_NAME} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH FALSE
    )
endif()

# ----------------------------
# Include directories
# ----------------------------

target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SQLITECPP_INCLUDE_DIR}
        ${SQLITECPP_SQLITE3_DIR}
)

if (TARGET Boost::headers)
    target_link_libraries(${PROJECT_NAME} PRIVATE Boost::headers)
elseif (Boost_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${Boost_INCLUDE_DIRS})
endif()

if (WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _WIN32_WINNT=0x0601)
endif()

# ----------------------------
# Libraries
# ----------------------------

target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
)

if (TARGET wx::core AND TARGET wx::base)
    target_link_libraries(${PROJECT_NAME} PRIVATE wx::core wx::base)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE ${wxWidgets_LIBRARIES})
endif()

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

install(CODE [[
    if(WIN32)
        set(runtime_pre_exclude_regexes
            "api-ms-.*"
            "ext-ms-.*"
        )
        set(runtime_post_exclude_regexes
            ".*[Ww]indows[\\/].*"
            ".*[Ss]ystem32[\\/].*"
        )
    elseif(APPLE)
        set(runtime_pre_exclude_regexes)
        set(runtime_post_exclude_regexes
            "^/System/.*"
            "^/usr/lib/.*"
        )
    elseif(UNIX)
        set(runtime_pre_exclude_regexes)
        set(runtime_post_exclude_regexes
            "^/lib/.*"
            "^/lib64/.*"
            "^/usr/lib/.*"
            "^/usr/lib64/.*"
        )
    endif()

    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "$<TARGET_FILE:test>"
        RESOLVED_DEPENDENCIES_VAR runtime_deps
        UNRESOLVED_DEPENDENCIES_VAR unresolved_deps
        PRE_EXCLUDE_REGEXES
            ${runtime_pre_exclude_regexes}
        POST_EXCLUDE_REGEXES
            ${runtime_post_exclude_regexes}
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
