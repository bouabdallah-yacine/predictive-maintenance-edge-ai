# Enables link-time optimisation (LTO): more compact code.
Import("env")
env.Append(CCFLAGS=["-flto"], LINKFLAGS=["-flto"])
