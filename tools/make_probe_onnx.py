# Builds a probe ONNX that ignores its input and always outputs a strong constant
# action, to prove the engine's inference -> actuator -> CharacterMovement pipeline:
# run it with -PursuitCharInference and the chaser must sprint in a straight line.
#
# Usage:
#   .venv\Scripts\python.exe tools\make_probe_onnx.py checkpoints\probe_constant.onnx

import sys

import numpy as np
import onnx
from onnx import helper, TensorProto


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: make_probe_onnx.py <out.onnx>")
        return 2

    # out = TargetSensor x W^T + B with W = ones(2,5), B = [1, 1]: any observation in
    # [-1,1]^5 produces an action with both components in [0, 3] - strongly non-zero.
    weight = np.ones((2, 5), dtype=np.float32)
    bias = np.ones((2,), dtype=np.float32)

    node = helper.make_node(
        "Gemm",
        ["TargetSensor", "probe_weight", "probe_bias"],
        ["CharMoveInput"],
        alpha=1.0, beta=1.0, transB=1,
    )

    graph = helper.make_graph(
        [node],
        "probe_constant",
        [helper.make_tensor_value_info("TargetSensor", TensorProto.FLOAT, ["batch_size", 5])],
        [helper.make_tensor_value_info("CharMoveInput", TensorProto.FLOAT, ["batch_size", 2])],
        [
            helper.make_tensor("probe_weight", TensorProto.FLOAT, [2, 5], weight.flatten().tolist()),
            helper.make_tensor("probe_bias", TensorProto.FLOAT, [2], bias.tolist()),
        ],
    )

    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, sys.argv[1])
    print(f"[make_probe_onnx] wrote {sys.argv[1]} (constant strong forward action)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
