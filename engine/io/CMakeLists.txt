# SPDX-License-Identifier: Apache-2.0
groomlab_fetch_alembic()

add_library(groomlab_alembic STATIC
    src/alembic_io.cpp
    src/alembic_mesh.cpp
    src/engine_export.cpp
    src/groom_scene.cpp
    src/import_curves_op.cpp
    src/guide_cache_op.cpp
    src/sim_cache.cpp
)
add_library(groomlab::alembic ALIAS groomlab_alembic)

target_include_directories(groomlab_alembic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(groomlab_alembic
    PUBLIC groomlab::core
    PRIVATE Alembic::Alembic groomlab::warnings)
