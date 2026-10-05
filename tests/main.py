import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
import QuantLib

iq = QuantLib.SPSCQueue(4)
iq.try_push(11)
item = iq.try_pop()

if __name__ == "__main__":
    print(item)
    