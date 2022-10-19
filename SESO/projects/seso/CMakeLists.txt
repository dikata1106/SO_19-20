cmake_minimum_required(VERSION 3.7.2)
project(SESO C) # create a new C project called 'Hello'
# add files to our project. Paths are relative to this file.
add_executable(SESO src/main.c)
# we need to link against the standard C lib for printf
target_link_libraries(SESO sel4muslcsys muslc)
# Set this image as the rootserver
DeclareRootserver(SESO)

