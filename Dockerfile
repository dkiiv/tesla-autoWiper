# Official Espressif IDF image — includes the full toolchain, CMake, ninja, esptool, etc.
# Pin to a specific IDF version for reproducible builds. Change as needed.
FROM espressif/idf:v5.3.2

# Install a few quality-of-life extras
RUN apt-get update && apt-get install -y --no-install-recommends \
    usbutils \
    && rm -rf /var/lib/apt/lists/*

# Default working directory maps to your project root on the host
WORKDIR /project

# The entrypoint uses the IDF environment wrapper so idf.py is always on PATH
ENTRYPOINT ["/opt/esp/entrypoint.sh"]
CMD ["/bin/bash"]
