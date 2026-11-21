import argparse
from pathlib import Path
import subprocess
import sys


IMAGE = "fizruk/stella@sha256:86d6123aff226787a02b1b8021c7d4f2cd3703586155f5a96d6f0a7c21d805c6"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--docker", default="docker")
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)

    args = parser.parse_args()

    try:
        with args.source.open("rb") as source:
            result = subprocess.run(
                (args.docker, "run", "--rm", "-i", IMAGE, "compile", "/dev/stdin", "-o", "/dev/stdout"),
                stdin=source, stdout=subprocess.PIPE, check=True,
            )

        if not result.stdout.strip():
            print(f"{args.source}: Stella compiler produced no C source", file=sys.stderr)
            return 1

        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(result.stdout)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"{args.source}: {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
