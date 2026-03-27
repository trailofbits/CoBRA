include(FetchContent)

FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.12.0
    GIT_SHALLOW    TRUE
)

# Tracy options — keep client lightweight.
set(TRACY_ENABLE      ON  CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND   ON  CACHE BOOL "" FORCE)
set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(tracy)
