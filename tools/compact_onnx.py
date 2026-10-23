# Halves the size of an ONNX model's weights where that loses nothing: every 32-bit float weight
# whose values all fit exactly in a 16-bit float is stored as 16-bit, with a Cast back to 32-bit in
# front of its users. ONNX Runtime folds those casts when it loads the model, so it runs on exactly
# the same 32-bit weights as before and gives bit-identical output. Weights that don't fit exactly
# stay as they are, so on a model without any it changes nothing.
#
# BS-RoFormer SW's weights were trained in 16-bit, so this takes its model from 209 MB to 105 MB.
#
# Usage: python compact_onnx.py <in.onnx> <out.onnx>   (in and out may be the same file)
# Needs: onnx, numpy.
import sys, numpy as np, onnx
from onnx import helper, numpy_helper

def compact(model):
    graph = model.graph
    inputs = {i.name for i in graph.input}
    used = {name for node in graph.node for name in node.input}
    casts, saved = [], 0
    for i, tensor in enumerate(graph.initializer):
        if tensor.data_type != onnx.TensorProto.FLOAT or tensor.name in inputs or tensor.name not in used:
            continue
        values = numpy_helper.to_array(tensor)
        half = values.astype(np.float16)
        if values.size < 16 or not np.array_equal(half.astype(np.float32), values):
            continue
        name = tensor.name
        graph.initializer[i].CopyFrom(numpy_helper.from_array(half, name + '_f16'))
        casts.append(helper.make_node('Cast', [name + '_f16'], [name], to=onnx.TensorProto.FLOAT, name=name + '_cast'))
        saved += values.nbytes // 2
    # The casts go first so every user of a weight still comes after the node that makes it.
    nodes = casts + list(graph.node)
    del graph.node[:]
    graph.node.extend(nodes)
    return saved

def main():
    source, out = sys.argv[1:3]
    model = onnx.load(source)
    saved = compact(model)
    if saved:
        onnx.checker.check_model(model, full_check=False)
        onnx.save(model, out)
    elif out != source:
        onnx.save(model, out)
    print(f'{source}: {saved / 1e6:.1f} MB smaller')

if __name__ == '__main__':
    main()
