"""Entry point for python -m agamemnon.orchestration invocation."""
from __future__ import annotations

import sys

from agamemnon.orchestration.daemon import main

sys.exit(main())
