cmake_minimum_required(VERSION 3.24 )

project(dicom2meshgui)

SET(CMAKE_INCLUDE_CURRENT_DIR ON)
SET(CMAKE_AUTOMOC ON)
SET(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets GLOBAL)
find_package(VTK 9 REQUIRED COMPONENTS GUISupportQt RenderingOpenGL2 RenderingCore InteractionStyle)

SET( APP_SOURCES d2mWidget.cpp dicomConverter.cpp sliceView.cpp brainExtractor.cpp)
SET( APP_INCLUDE_DIRS . )

include_directories( ${APP_INCLUDE_DIRS} )

add_library(dicom2meshgui_widgets STATIC ${APP_SOURCES})
target_link_libraries(dicom2meshgui_widgets PUBLIC dicom2meshlib Qt6::Widgets Qt6::OpenGLWidgets VTK::GUISupportQt)
target_include_directories(dicom2meshgui_widgets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(dicom2meshgui_widgets PUBLIC cxx_std_17)
target_compile_definitions(dicom2meshgui_widgets PRIVATE D2M_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
add_executable(dicom2meshgui main.cpp rsc/dicom2meshgui.qrc)
target_link_libraries(dicom2meshgui PRIVATE dicom2meshgui_widgets)
vtk_module_autoinit(TARGETS dicom2meshgui MODULES ${VTK_LIBRARIES})
target_compile_features(dicom2meshgui PRIVATE cxx_std_17 )
if(APPLE)
    set_target_properties(dicom2meshgui PROPERTIES MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME DicomToMesh
        MACOSX_BUNDLE_GUI_IDENTIFIER org.dicomtomesh.app)
endif()

if(TESTDICOM2MESHLIB OR TESTDICOM2MESH)
    find_package(Qt6 REQUIRED COMPONENTS Test GLOBAL)
    add_executable(runGuiTests test/t_gui.cpp)
    target_include_directories(runGuiTests PRIVATE ${CMAKE_SOURCE_DIR}/lib/test)
    target_link_libraries(runGuiTests PRIVATE dicom2meshgui_widgets Qt6::Test)
endif()
