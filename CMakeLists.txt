cmake_minimum_required(VERSION 2.8)
project(final C)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c11 -pthread -g")

add_executable(final main.c)
