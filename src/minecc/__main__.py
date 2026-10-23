"""CLI entrypoint for `python3 -m minecc`."""

from __future__ import annotations

import sys

from minecc.cli import main

if __name__ == "__main__":
    sys.exit(main())
