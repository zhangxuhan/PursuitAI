"""Convert a raw Schola PPO ONNX export to SB3 deterministic Box actions.

This checkpoint uses an unsquashed Gaussian policy. SB3 predict() clips the
raw deterministic mean to the action Box: move [0, 1], jump [-1, 1].
Neither a Tanh nor an affine unscale belongs in this particular graph.
"""
import argparse

import numpy as np
import onnx
from onnx import helper, numpy_helper


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("destination")
    args = parser.parse_args()
    model = onnx.load(args.source)
    graph = model.graph
    expected = {"CharMoveInput": (2, 0.0, 1.0), "JumpInput": (1, -1.0, 1.0)}
    outputs = {output.name: output for output in graph.output}
    if set(outputs) != set(expected):
        raise ValueError(f"unexpected outputs: {list(outputs)}")

    for name, (width, low, high) in expected.items():
        output = outputs[name]
        dims = output.type.tensor_type.shape.dim
        if len(dims) != 2 or dims[1].dim_value != width:
            raise ValueError(f"unexpected shape for {name}: {dims}")
        producer = [node for node in graph.node if name in node.output]
        if len(producer) != 1:
            raise ValueError(f"expected one producer for {name}")
        raw_name = name + "_raw"
        producer[0].output[list(producer[0].output).index(name)] = raw_name
        low_name, high_name = name + "_low", name + "_high"
        graph.initializer.append(numpy_helper.from_array(np.array(low, dtype=np.float32), low_name))
        graph.initializer.append(numpy_helper.from_array(np.array(high, dtype=np.float32), high_name))
        graph.node.append(helper.make_node("Clip", [raw_name, low_name, high_name], [name], name=name + "_clip"))

    onnx.checker.check_model(model)
    onnx.save(model, args.destination)
    print(f"saved {args.destination}")


if __name__ == "__main__":
    main()
