# The image tools/build_xlate.py --android runs in: the .NET 10 SDK on arm64 Linux with clang and lld (docs/building.md,
# "Android"). NativeAOT builds for the OS it runs on, so the Android translator is linked here, against the NDK's
# sysroot mounted from the host.
FROM mcr.microsoft.com/dotnet/sdk:10.0
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        clang lld llvm python3 git \
    && rm -rf /var/lib/apt/lists/*
