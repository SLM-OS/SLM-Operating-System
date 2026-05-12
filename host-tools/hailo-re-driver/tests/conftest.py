"""Make `hailo_re_driver` importable when the package isn't pip-installed.

Lets `python -m unittest discover -s tests` and `pytest tests` both work
without requiring `pip install -e .` first.
"""

import sys
from pathlib import Path

_PKG_ROOT = Path(__file__).resolve().parent.parent
if str(_PKG_ROOT) not in sys.path:
    sys.path.insert(0, str(_PKG_ROOT))
