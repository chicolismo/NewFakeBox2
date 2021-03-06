cmake_minimum_required(VERSION 2.8)
project(DropBox)

SET(CMAKE_CXX_FLAGS "-std=c++11 -lssl -lcrypto")

set(SERVER_SOURCE_FILES dropboxServer.cpp dropboxServer.h dropboxUtil.cpp dropboxUtil.h)
set(CLIENT_SOURCE_FILES dropboxClient.cpp dropboxClient.h dropboxUtil.cpp dropboxUtil.h
        Inotify-master/FileSystemEvent.h Inotify-master/Inotify.h front_end.cpp front_end.h)

find_package(Boost COMPONENTS system filesystem regex REQUIRED)
find_package(Threads)
find_package(OpenSSL)

if (Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
    include_directories(${OPENSSL_INCLUDE_DIR})

    add_executable(server ${SERVER_SOURCE_FILES})
    target_link_libraries(server ${Boost_LIBRARIES})
    target_link_libraries(server ${OPENSSL_LIBRARIES})
    target_link_libraries(server ${CMAKE_THREAD_LIBS_INIT})


    add_executable(client ${CLIENT_SOURCE_FILES})
    target_link_libraries(client ${Boost_LIBRARIES})
    target_link_libraries(client ${OPENSSL_LIBRARIES})
    target_link_libraries(client ${CMAKE_THREAD_LIBS_INIT})

else ()
    message(FATAL_ERROR "Could not find Boost!")
endif ()
