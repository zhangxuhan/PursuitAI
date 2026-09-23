# Probe an exported policy ONNX: report graph inputs/outputs and run one forward
# pass with plausible observation values, so "policy outputs ~0" (a weak model) can
# be told apart from "the engine never applies the action" (a pipeline bug).
#
# Usage:
#   .venv\Scripts\python.exe tools\inspect_onnx_policy.py checkpoints\policy_v2_4k.onnx

import sys

import numpy as np
import onnx
from onnx.reference import ReferenceEvaluator


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: inspect_onnx_policy.py <model.onnx>")
        return 2

    model = onnx.load(sys.argv[1])
    graph = model.graph

    print("== inputs ==")
    feeds = {}
    for inp in graph.input:
        dims = inp.type.tensor_type.shape.dim
        shape = [1 if (d.dim_param or d.dim_value == 0) else d.dim_value for d in dims]
        last = shape[-1] if shape else 0
        print(f"  {inp.name}  shape={shape}")
        # Sensor observations live in [-1, 1] by design; a mid-range forward chase
        # (unit forward vector, mid distance, zero speeds) is a plausible probe.
        feeds[inp.name] = (
            np.array([[0.7, 0.7, 0.5, 0.0, 0.0]], dtype=np.float32).reshape(shape)
            if last == 5
            else np.zeros(shape, dtype=np.float32)
        )

    print("== outputs ==")
    for out in graph.output:
        shape = [d.dim_param or d.dim_value for d in out.type.tensor_type.shape.dim]
        print(f"  {out.name}  shape={shape}")

    session = ReferenceEvaluator(model)
    results = session.run(None, feeds)
    print("== forward pass ==")
    for out, value in zip(graph.output, results):
        print(f"  {out.name} = {np.array2string(np.asarray(value), precision=4)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
