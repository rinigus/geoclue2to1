# Test client application
add_executable(geoclue2-test-client
    geoclue2-test-client.c
)

target_include_directories(geoclue2-test-client PRIVATE
    ${GIO_INCLUDE_DIRS}
)

target_link_libraries(geoclue2-test-client PRIVATE
    ${GIO_LIBRARIES}
)

install(TARGETS geoclue2-test-client
    RUNTIME DESTINATION bin
)