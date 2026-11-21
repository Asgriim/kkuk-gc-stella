import argparse
from dataclasses import dataclass
import fnmatch
import json
import os
from pathlib import Path
import subprocess
import sys


@dataclass(frozen=True)
class Case:
    name: str
    command: tuple[str, ...]
    young: str | None
    error: str | None = None
    stdout: str = "ok\n"
    minimum_statistics: tuple[tuple[str, int], ...] = ()


def abi_cases(bin_dir: Path) -> list[Case]:
    def executable(name: str) -> str:
        return str(bin_dir / f"gc_test_{name}{'.exe' if os.name == 'nt' else ''}")

    cases = []
    portable = ("graph", "layouts", "roots", "update", "half_heap")

    for heap in (2048, 4096):
        for young in (None, "1", "24", "32", "64", "128", "511", str(heap)):
            for name in portable:
                cases.append(Case(
                    f"abi/{name}/heap={heap}/young={young or 'default'}",
                    (executable(name), str(heap)), young,
                ))

    for name in ("barrier", "update_barrier", "large_init"):
        cases.append(Case(f"generations/{name}", (executable(name), "2048"), "128"))

    cases.append(Case("diagnostics/output", (executable("diagnostics"), "2048"), "128"))

    for young in ("1", "128", "2048"):
        cases.append(Case(f"abi/c_source/young={young}", (executable("c_abi_smoke"),), young))

    accesses = (
        "init_managed", "init_primitive", "read_managed", "write_managed",
        "read_primitive", "write_primitive",
    )
    operations = (
        "shutdown", "alloc", "push_root", "pop_root", "permanent_root",
        "update", "statistics", "state", "roots", *accesses,
    )

    def add_errors(category: str, names: tuple[str, ...], prefix: str = "",
                   error: str = "ABI violation:") -> None:
        command = executable(f"error_{category}")

        for name in names:
            path = f"{prefix}/{name}" if prefix else name
            cases.append(Case(
                f"abi/errors/{path}", (command, name, "2048"), "128", error, stdout="",
            ))

    add_errors("lifecycle", tuple(f"before/{name}" for name in operations))
    add_errors("lifecycle", tuple(f"after/{name}" for name in ("init", *operations)))

    add_errors("config", ("null", "version", "zero"), "config")
    add_errors("lifecycle", ("repeat_init",))

    add_errors("roots", (
        "shutdown_roots", "empty_pop", "root_order",
        "duplicate_root", "duplicate_permanent", "null_root",
    ))

    add_errors("descriptors", (
        "null", "version", "static", "capacity", "count_overflow",
        "multiply_overflow", "header_overflow",
    ), "descriptor")

    add_errors("access", tuple(f"{prefix}/{name}" for prefix in ("null", "index") for name in accesses))
    add_errors("access", (
        "static/init_managed", "static/init_primitive",
        "static/write_managed", "static/write_primitive",
    ))

    add_errors("update", (
        "null_object", "immutable", "static", "capacity", "null_descriptor",
        "missing_managed", "missing_primitive",
    ), "update")

    add_errors("oom", ("object", "live"), "oom", "cannot allocate")

    invalid_young = (
        ("empty", ""), ("zero", "0"), ("negative", "-1"), ("plus", "+32"),
        ("letters", "abc"), ("suffix", "32x"), ("leading_space", " 32"),
        ("trailing_space", "32 "), ("above_heap", "2049"),
        ("overflow", "18446744073709551616"),
    )

    for label, young in invalid_young:
        cases.append(Case(
            f"generations/errors/young/{label}", (executable("error_config"), "environment", "2048"),
            young, "ABI violation:", stdout="",
        ))

    return cases


def stella_cases(bin_dir: Path) -> list[Case]:
    cases = []
    params_dir = Path(__file__).parent / "stella" / "params"

    for file in sorted(params_dir.glob("*.json")):
        settings = json.loads(file.read_text(encoding="utf-8"))
        suffix = ".exe" if os.name == "nt" else ""
        executable = bin_dir / f"stella_test_{settings['program']}{suffix}"

        cases.append(Case(
            f"stella/{file.stem}", (str(executable), settings["argument"], str(settings["heap"])),
            str(settings["young"]) if "young" in settings else None,
            settings.get("error"), settings["stdout"],
            tuple(settings.get("minimum_statistics", {}).items()),
        ))

    return cases


def read_statistics(stderr: str) -> dict[str, int]:
    statistics = {}

    for line in stderr.splitlines():
        name, separator, value = line.partition(": ")

        if not separator or not value.isascii() or not value.isdecimal() or name in statistics:
            raise ValueError(f"malformed statistics line: {line!r}")

        statistics[name] = int(value)

    return statistics


def run_case(case: Case) -> str | None:
    env = os.environ.copy()

    for name in ("STELLA_GC_YOUNG_BYTES", "STELLA_GC_STATS", "STELLA_GC_STATE"):
        env.pop(name, None)

    if case.young is not None:
        env["STELLA_GC_YOUNG_BYTES"] = case.young

    if case.minimum_statistics:
        env["STELLA_GC_STATS"] = "1"

    try:
        result = subprocess.run(case.command, env=env, capture_output=True, text=True, timeout=20)
    except (subprocess.TimeoutExpired, OSError) as error:
        return str(error)

    sanitizer_markers = ("AddressSanitizer", "UndefinedBehaviorSanitizer", "runtime error:", "LeakSanitizer")
    if any(marker in result.stderr for marker in sanitizer_markers):
        return f"sanitizer diagnostic\n{result.stderr}"

    if case.error is None:
        passed = result.returncode == 0 and result.stdout == case.stdout
        if passed and case.minimum_statistics:
            try:
                statistics = read_statistics(result.stderr)
            except ValueError as error:
                return str(error)

            for name, minimum in case.minimum_statistics:
                if statistics.get(name, -1) < minimum:
                    return f"expected {name!r} >= {minimum}, got {statistics.get(name)!r}"
        elif not case.minimum_statistics:
            passed = passed and not result.stderr
    else:
        passed = (
            result.returncode == 1
            and result.stdout == case.stdout
            and result.stderr.startswith("stella-c-gc-abi-v1: ")
            and case.error in result.stderr
        )

    if not passed:
        return f"exit={result.returncode}\nstdout={result.stdout!r}\nstderr={result.stderr!r}"

    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path)
    parser.add_argument("--suite", choices=("abi", "stella", "all"), default="abi")
    parser.add_argument("--list-names", action="store_true")
    parser.add_argument("--case")
    parser.add_argument("--filter", default="*")
    parser.add_argument("--verbose", action="store_true")

    args = parser.parse_args()

    if not args.list_names and args.bin_dir is None:
        parser.error("--bin-dir is required to run cases")

    bin_dir = args.bin_dir.resolve() if args.bin_dir else Path()
    cases = []

    if args.suite in ("abi", "all"):
        cases.extend(abi_cases(bin_dir))
    if args.suite in ("stella", "all"):
        cases.extend(stella_cases(bin_dir))

    names = [case.name for case in cases]
    if len(names) != len(set(names)):
        parser.error("case names must be unique")

    if args.list_names:
        print("\n".join(names))
        return 0

    if args.case is not None:
        cases = [case for case in cases if case.name == args.case]
    else:
        cases = [case for case in cases if fnmatch.fnmatchcase(case.name, args.filter)]

    if not cases:
        parser.error("no matching cases")

    failures = 0

    for case in cases:
        error = run_case(case)
        if error:
            failures += 1
            print(f"FAIL {case.name}\n{error}", flush=True)
        elif args.verbose:
            print(f"PASS {case.name}", flush=True)

    print(f"{len(cases) - failures}/{len(cases)} passed", flush=True)

    return int(failures != 0)


if __name__ == "__main__":
    sys.exit(main())
