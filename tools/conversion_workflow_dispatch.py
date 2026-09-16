#!/usr/bin/env python3
"""Dispatch the admitted model-conversion workflow by backend family."""

from __future__ import annotations

import sys
from pathlib import Path

import agent_workflow as core


def _contract_argument(arguments: list[str]) -> str:
    for index, argument in enumerate(arguments):
        if argument == "--contract" and index + 1 < len(arguments):
            return arguments[index + 1]
        if argument.startswith("--contract="):
            return argument.split("=", 1)[1]
    raise core.WorkflowError("--contract is required")


def workflow_family(arguments: list[str]) -> str:
    contract_path, _, contract = core.resolve_contract_context(
        Path(_contract_argument(arguments))
    )
    del contract_path
    target_chip = str(contract.get("parameters", {}).get("targetChip", ""))
    family = core._conversion_toolchain_family(target_chip)
    if family not in {"sophon", "rknn"}:
        raise core.WorkflowError(
            f"no conversion workflow is registered for targetChip={target_chip or 'unspecified'}"
        )
    return family


def main(arguments: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if arguments is None else arguments)
    if not args or args[0] not in {"convert", "verify"}:
        print("usage: conversion_workflow_dispatch.py convert|verify --contract ...", file=sys.stderr)
        return 2
    try:
        family = workflow_family(args[1:])
        if family == "rknn":
            from rknn import agent_conversion_workflow as rknn_conversion

            return rknn_conversion.main(args)
        import model_conversion_workflow as sophon_conversion

        return sophon_conversion.main(args)
    except core.WorkflowError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
