# Active l'optimisation à l'édition de liens (LTO) : code plus compact.
Import("env")
env.Append(CCFLAGS=["-flto"], LINKFLAGS=["-flto"])
