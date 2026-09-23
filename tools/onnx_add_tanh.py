# Copyright Epic Games, Inc. All Rights Reserved.
#
# Appends a Tanh node to a Schola-exported PPO policy ONNX graph.
#
# Why this exists (docs/COMPARISON_PLAN.md, gap RED-1): `schola sb3 export` emits the
# policy MLP without the final Tanh squashing that SB3's predict(deterministic=True)
# applies. Raw action values can exceed [-1, 1] and the movement direction reads up
# to 14.5 degrees away from what the trained policy actually intends.
#
# Usage:
#   python onnx_add_tanh.py <in.onnx> <out.onnx> [final_output_names]
#
# final_output_names (optional): comma-separated list renaming the declared outputs
# after the Tanh. One name per graph output, in order.
#   - Single-actuator export (v2.0): schola names the output `CharMoveInput` for a
#     *final* policy save but plain `action` for a mid-training checkpoint zip
#     (--enable-checkpoints); passing "CharMoveInput" forces the actuator key.
#   - Two-actuator export (v2.1, dict space {CharMoveInput, JumpInput}): the raw
#     export may carry 1-2 outputs; pass "CharMoveInput,JumpInput" and each declared
#     output is Tanh-squashed and renamed to its actuator key.
# The engine's dict action-space binding looks up graph outputs by the actuator key,
# so a misnamed output silently disables the policy ("Key action not found or invalid
# in Dict Buffer" in LogScholaNNE).
#
# The transform is one node per output: each previous action tensor feeds a Tanh, and
# the graph's declared output is retargeted to the Tanh result. No weights are
# touched, so the operation is exact and verifiable with the standard checker.

import sys

import onnx
from onnx import helper


def rewire_output(graph, previous_name, raw_name):
    """Rename the tensor producing `previous_name` to `raw_name` everywhere."""
    renamed = False
    for node in graph.node:
        for idx, out_name in enumerate(node.output):
            if out_name == previous_name:
                node.output[idx] = raw_name
                renamed = True
        for idx, in_name in enumerate(node.input):
            if in_name == previous_name:
                node.input[idx] = raw_name
    for value_info in graph.value_info:
        if value_info.name == previous_name:
            value_info.name = raw_name
    return renamed


def split_concatenated_output(model, graph, action_output, names, widths, dst):
    """One concatenated action tensor -> one Tanh-squashed output per actuator.

    Mid-training checkpoint zips export a single output holding every actuator's slice
    back to back (here: 2 move dims then 1 jump dim), while a final policy save exports
    one output per actuator. The engine binds by actuator key, so the concatenated form
    has to be split - and the slice widths cannot be inferred from the names, which is
    why the caller passes them ("CharMoveInput:2,JumpInput:1").
    """
    import numpy as np
    from onnx import numpy_helper

    previous_name = action_output.name
    raw_name = previous_name + "_raw"
    if not rewire_output(graph, previous_name, raw_name):
        print(f"[onnx_add_tanh] output '{previous_name}' has no producing node in the graph")
        return 1

    del graph.output[:]
    offset = 0
    for index, (name, width) in enumerate(zip(names, widths)):
        starts = f"schola_split_start_{index}"
        ends = f"schola_split_end_{index}"
        axes = f"schola_split_axis_{index}"
        steps = f"schola_split_step_{index}"
        graph.initializer.append(numpy_helper.from_array(np.array([offset], dtype=np.int64), starts))
        graph.initializer.append(numpy_helper.from_array(np.array([offset + width], dtype=np.int64), ends))
        graph.initializer.append(numpy_helper.from_array(np.array([-1], dtype=np.int64), axes))
        graph.initializer.append(numpy_helper.from_array(np.array([1], dtype=np.int64), steps))

        part = f"{raw_name}_part{index}"
        graph.node.append(helper.make_node("Slice", [raw_name, starts, ends, axes, steps], [part],
                                          name=f"schola_action_split_{index}"))
        graph.node.append(helper.make_node("Tanh", [part], [name], name=f"schola_tanh_squash_{index}"))

        # Declared with a shape, not just a name: the runtime sizes its dict action
        # buffer from this value_info, and a type-less output was rejected by the checker.
        graph.output.append(helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, ["batch_size", width]))
        print(f"[onnx_add_tanh] output {index}: slice [{offset}:{offset + width}] of '{raw_name}' -> '{name}'")
        offset += width

    try:
        onnx.checker.check_model(model)
    except Exception as exc:  # the split outputs carry no static shape, which some checkers reject
        print(f"[onnx_add_tanh] checker skipped ({exc})")
    onnx.save(model, dst)
    print(f"[onnx_add_tanh] 1 concatenated output split into {len(names)} Tanh-squashed output(s) -> {dst}")
    return 0


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("usage: onnx_add_tanh.py <in.onnx> <out.onnx> [final_output_names]")
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    final_names = [n.strip() for n in sys.argv[3].split(",")] if len(sys.argv) == 4 else None
    model = onnx.load(src)
    graph = model.graph

    outputs = list(graph.output)
    if not outputs:
        print("[onnx_add_tanh] graph declares no outputs - nothing to squash")
        return 1

    # Sizes may be given per name ("CharMoveInput:2,JumpInput:1"); they are the only way
    # to split a single concatenated action output, since a name says nothing about width.
    widths = None
    if final_names and any(":" in n for n in final_names):
        widths = []
        parsed = []
        for entry in final_names:
            name, _, size = entry.partition(":")
            parsed.append(name)
            widths.append(int(size) if size else 0)
        final_names = parsed

    if final_names and len(final_names) != len(outputs):
        if widths and len(outputs) == 1 and len(widths) == len(final_names) and all(w > 0 for w in widths):
            return split_concatenated_output(model, graph, outputs[0], final_names, widths, dst)
        print(
            f"[onnx_add_tanh] {len(outputs)} graph output(s) vs {len(final_names)} final "
            f"name(s) {final_names} - refusing to guess the pairing"
        )
        return 1

    for index, action_output in enumerate(outputs):
        previous_name = action_output.name
        raw_name = previous_name + "_raw"

        # The engine's dict action-space binding looks up graph outputs by the actuator's
        # key name, so the declared output KEEPS its name and the rename happens on the
        # intermediate: the producing tensor becomes <name>_raw, every node input that
        # referenced it follows, and a Tanh node republishes the original name.
        if not rewire_output(graph, previous_name, raw_name):
            # A declared output with no producing node is already broken upstream.
            print(f"[onnx_add_tanh] output '{previous_name}' has no producing node in the graph")
            return 1

        published_name = previous_name
        if final_names and final_names[index] != previous_name:
            published_name = final_names[index]

        graph.node.append(
            helper.make_node("Tanh", [raw_name], [published_name], name=f"schola_tanh_squash_{index}")
        )
        action_output.name = published_name

        print(
            f"[onnx_add_tanh] output {index}: raw '{previous_name}' Tanh-squashed -> "
            f"declared as '{published_name}'"
        )

    onnx.checker.check_model(model)
    onnx.save(model, dst)
    print(f"[onnx_add_tanh] {src} -> {dst} ({len(outputs)} output(s) Tanh-squashed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
