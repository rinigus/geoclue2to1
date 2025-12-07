# Generated D-Bus glue code
set(GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated)
set(GENERATED_SOURCES
    ${GENERATED_DIR}/geoclue2-manager.c
    ${GENERATED_DIR}/geoclue2-client.c
    ${GENERATED_DIR}/geoclue2-location.c
)

add_executable(geoclue2to1
    main.cpp
    geoclue2_manager.cpp
    geoclue2_client.cpp
    geoclue2_location.cpp
    geoclue1_backend.cpp
    ${GENERATED_SOURCES}
)

target_include_directories(geoclue2to1 PRIVATE
    ${GIO_INCLUDE_DIRS}
    ${GENERATED_DIR}
)

target_link_libraries(geoclue2to1 PRIVATE
    ${GIO_LIBRARIES}
)

install(TARGETS geoclue2to1
    RUNTIME DESTINATION bin
)
