"""Entry point for python -m keystone invocation."""
from __future__ import annotations

import sys

from keystone.daemon import main

sys.exit(main())
