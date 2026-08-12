#!/usr/bin/env bash
# /// script
# requires-python = ">=3.12"
# dependencies = ["idapro"]
# ///
''''command -v uv >/dev/null 2>&1 && exec uv run "$0" "$@"; exec nix shell nixpkgs#uv --command uv run "$0" "$@" #'''
"""Headless CoBRA runner using idalib.

Decompiles every function in the given IDA database and prints the ones
where CoBRA simplified an MBA expression.
"""
import argparse
import sys
from pathlib import Path
import idapro  # noqa: E402

COBRA_TAG = ord('C')


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Decompile functions from an IDA database with CoBRA loaded."
    )
    parser.add_argument("binary", type=Path, help="Path to binary to analyze")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    idapro.enable_console_messages(True)

    rc = idapro.open_database(str(args.binary), True)
    if rc != 0:
        print(f"ERROR: open_database returned {rc}")
        return 1

    import ida_auto  # noqa: E402
    import ida_funcs  # noqa: E402
    import ida_hexrays  # noqa: E402
    import ida_netnode  # noqa: E402
    import ida_segment  # noqa: E402
    import idautils  # noqa: E402

    ida_auto.auto_wait()

    if not ida_hexrays.init_hexrays_plugin():
        print("ERROR: Hex-Rays not available")
        return 1

    for ea in idautils.Functions():
        func = ida_funcs.get_func(ea)
        if func is None:
            continue
        if func.flags & (ida_funcs.FUNC_THUNK | ida_funcs.FUNC_LIB | ida_funcs.FUNC_NORET):
            continue
        seg = ida_segment.getseg(ea)
        if seg and (seg.type == ida_segment.SEG_XTRN
                    or ida_segment.get_segm_name(seg) in (".plt", ".plt.sec", ".plt.got")):
            continue

        name = ida_funcs.get_func_name(ea)
        hf = ida_hexrays.hexrays_failure_t()
        try:
            cfunc = ida_hexrays.decompile(ea, hf)
        except Exception:
            cfunc = None
        if cfunc is None:
            continue

        # Check the netnode tag set by CoBRA when it simplifies a function.
        n = ida_netnode.netnode(ea)
        simplified = n.altval(0, COBRA_TAG)

        if not simplified:
            continue

        print(f"--- {name} @ {ea:#x} ---")
        print(f"  [CoBRA: simplified {simplified} expression(s)]")
        print(str(cfunc).strip())
        print()

    idapro.close_database(False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
