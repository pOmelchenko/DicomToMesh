cmake_minimum_required( VERSION 3.24 )

project( d2m )

option(USE_VTK_DICOM "Use VTK-DICOM Lib (extended DICOM support)" OFF)

# Request only the modules used by DicomToMesh. Loading every VTK module
# also requires unrelated SDKs (Python, Boost, databases, etc.). Keep the
# original discovery path for legacy VTK and optional vtk-dicom builds.
if(NOT USE_VTK_DICOM)
    find_package(VTK 9 QUIET COMPONENTS
        CommonCore CommonDataModel CommonExecutionModel CommonMath CommonTransforms
        FiltersCore FiltersGeneral FiltersSources ImagingCore
        IOGeometry IOImage IOPLY InteractionStyle
        RenderingCore RenderingOpenGL2 RenderingVolume RenderingVolumeOpenGL2
    )
endif()
if(USE_VTK_DICOM OR NOT VTK_FOUND)
    find_package(VTK REQUIRED)
endif()
if(VTK_MAJOR_VERSION LESS 9)
    include(${VTK_USE_FILE})
endif()

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



