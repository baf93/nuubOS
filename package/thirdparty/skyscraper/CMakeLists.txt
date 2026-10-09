# nuubOS build of Skyscraper (upstream uses qmake: skyscraper.pro). Same
# sources and defines; resources.qrc is bundled so the binary runs without
# installed configuration files (Skyscraper's "single binary" setup).
cmake_minimum_required(VERSION 3.16)
project(skyscraper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Sql Xml)

file(GLOB SKYSCRAPER_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB SKYSCRAPER_HEADERS ${CMAKE_SOURCE_DIR}/src/*.h)

add_executable(Skyscraper ${SKYSCRAPER_SOURCES} ${SKYSCRAPER_HEADERS}
	${CMAKE_SOURCE_DIR}/resources.qrc)
target_include_directories(Skyscraper PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
# SYSCONFDIR does not exist on nuubOS: Skyscraper then loads its
# configuration from the bundled resources.
target_compile_definitions(Skyscraper PRIVATE
	VERSION="${SKYSCRAPER_VERSION}"
	PREFIX="/usr"
	SYSCONFDIR="/usr/share/skyscraper"
	QT_NO_DEBUG_OUTPUT)
target_link_libraries(Skyscraper PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Xml)

install(TARGETS Skyscraper RUNTIME DESTINATION bin)
