import sys
from pathlib import Path

build_dir = str(Path(__file__).resolve().parents[1] / "build")
sys.path.insert(0, build_dir)

from PyQuantLib import ISPSCQueue

iq = ISPSCQueue(4)
iq.try_push(11)
item = iq.try_pop()

if __name__ == "__main__":
    print(item)
    