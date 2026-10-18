import sys
import os
from pathlib import Path

build_dir = str(Path(__file__).resolve().parents[1] / "build")
sys.path.insert(0, build_dir)

# Add DLL directories for Windows
if hasattr(os, 'add_dll_directory'):
    os.add_dll_directory(build_dir)
    os.add_dll_directory("C:/vcpkg/installed/x64-windows/bin")

from PyQuantLib import ISPSCQueue

iq = ISPSCQueue(4)
iq.try_push(11)
item = iq.try_pop()

if __name__ == "__main__":
    print(item)
    