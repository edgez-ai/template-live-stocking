Import("env")

# Code Server may launch PlatformIO from a generated build directory in a
# multi-root workspace. Pin esptool's latency configuration to the project so
# toolbar uploads behave exactly like `pio run` from the firmware directory.
env["ENV"]["ESPTOOL_CFGFILE"] = env.subst("$PROJECT_DIR/esptool.cfg")
