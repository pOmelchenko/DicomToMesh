cmake_minimum_required( VERSION 3.24 )

project( d2m )

find_package(DCMTK CONFIG REQUIRED)

option(USE_VTK_DICOM "Deprecated: validated DCMTK loading is always enabled" OFF)
if(USE_VTK_DICOM)
    message(DEPRECATION "USE_VTK_DICOM is obsolete. Both GUI and CLI now use the validated DCMTK loader.")
endif()

# Request only the modules used by DicomToMesh. Loading every VTK module
# also requires unrelated SDKs (Python, Boost, databases, etc.).
find_package(VTK 9 REQUIRED COMPONENTS
    CommonCore CommonDataModel CommonExecutionModel CommonMath CommonTransforms
    FiltersCore FiltersGeneral FiltersSources ImagingCore
    IOGeometry IOImage IOPLY InteractionStyle
    RenderingCore RenderingOpenGL2 RenderingVolume RenderingVolumeOpenGL2
)

option(GCOV "COVERAGE" OFF)
IF(GCOV)
    message(STATUS "COVERAGE ON")
    set(COVERAGE_COMPILER_FLAGS "-fprofile-arcs -ftest-coverage -O0" CACHE INTERNAL "")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COVERAGE_COMPILER_FLAGS}")
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COVERAGE_COMPILER_FLAGS}")
ENDIF()

add_subdirectory(lib)
add_subdirectory(dicom2mesh)

option(BUILD_GUI "Build Dicom2Mesh GUI" OFF)
if(BUILD_GUI)
    add_subdirectory(gui)
endif(BUILD_GUI)



