cmake_minimum_required(VERSION 3.21)
project(QuestBookTests LANGUAGES CXX RC)
enable_testing()

add_executable(list-navigation list_navigation.cpp)
target_compile_features(list-navigation PRIVATE cxx_std_17)
add_test(NAME list-navigation COMMAND list-navigation)

add_executable(mouse-input mouse_input.cpp ../src/quest_cursor.rc)
target_include_directories(mouse-input PRIVATE ../src)
target_compile_features(mouse-input PRIVATE cxx_std_17)
add_test(NAME mouse-input COMMAND mouse-input)
