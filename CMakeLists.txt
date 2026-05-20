cmake_minimum_required(VERSION 4.0)

set(NAME "MCG_PC") # Глобальное имя проекта


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/libs) # Место хранения скомпилированных библиотек

project(${NAME})

# Опции для GLFW
option(GLFW_BUILD_EXAMPLES "Build the GLFW example programs" OFF)
option(GLFW_BUILD_TESTS "Build the GLFW test programs" OFF)
option(GLFW_BUILD_DOCS "Build the GLFW documentation" OFF)
option(GLFW_INSTALL "Generate installation target" OFF)

# Подключаем все файлы, включая заголовочные .h
include_directories("include")
include_directories("third-party/stb")
add_subdirectory(third-party/GLFW EXCLUDE_FROM_ALL) # Добавляем GLFW как подпроект


# Создаем .exe файл
add_executable(${NAME}
        "src/main.cpp"
)

add_library(glad "include/glad" "src/glad.c") # Нужен ли тут inlude/glad????

# Подключение библиотек
target_link_libraries(${NAME}
        glfw
        glad
)