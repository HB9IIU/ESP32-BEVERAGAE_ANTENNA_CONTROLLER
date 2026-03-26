Import("env")
import os
import shutil

print(">>> extra_script.py (OTA-only) loaded", flush=True)

def after_build(source, target, env):
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")

    # Firmware produced by PlatformIO
    firmware_bin = os.path.join(build_dir, f"{env.subst('$PROGNAME')}.bin")

    # Find main .cpp file in src/
    src_dir = os.path.join(project_dir, "src")
    cpp_files = [f for f in os.listdir(src_dir) if f.endswith(".cpp")]

    main_name = os.path.splitext(cpp_files[0])[0] if cpp_files else "firmware"

    dest_dir  = os.path.join(project_dir, "firmware")
    dest_file = os.path.join(dest_dir, f"{main_name}_OTA.bin")

    os.makedirs(dest_dir, exist_ok=True)

    try:
        shutil.copyfile(firmware_bin, dest_file)
        size = os.path.getsize(dest_file) / (1024 * 1024)
        print(f">>> OTA firmware ready: {dest_file} ({size:.2f} MB)", flush=True)
    except Exception as e:
        print(f"!!! OTA copy failed: {e}", flush=True)

env.AddPostAction("buildprog", after_build)
