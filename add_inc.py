Import("env")
import os
# Add the project 'include' directory
env.Append(CPPPATH=[os.path.join(env.get("PROJECT_DIR"), "include")])

# Add libdeps directory so __has_include("lvgl/lvgl.h") works for M5GFX
libdeps_path = os.path.join(env.get("PROJECT_DIR"), ".pio", "libdeps", env.get("PIOENV"))
env.Append(CPPPATH=[libdeps_path])
