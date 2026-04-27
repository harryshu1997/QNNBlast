#!/usr/bin/env python3
"""Build a minimal ONNX MatMul model: y[1,M] = x[1,K] @ W[K,M], FP16.

Used for QNN-vs-qblast comparison. Picks FP16 because that's what QNN's
"stock" MatMul lands on for HTP without explicit quantization, matching
plan §319's "≥ 2× over QNN's stock MatMul" baseline expectation.
"""

import argparse
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

ap = argparse.ArgumentParser()
ap.add_argument('--M', type=int, required=True)
ap.add_argument('--K', type=int, required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--seed', type=int, default=1234)
args = ap.parse_args()

rng = np.random.default_rng(args.seed)
W = rng.standard_normal((args.K, args.M)).astype(np.float16) * 0.05  # bounded scale

x_in = helper.make_tensor_value_info('x', TensorProto.FLOAT16, [1, args.K])
y_out = helper.make_tensor_value_info('y', TensorProto.FLOAT16, [1, args.M])
W_init = numpy_helper.from_array(W, name='W')

node = helper.make_node('MatMul', ['x', 'W'], ['y'])
graph = helper.make_graph([node], f'matmul_{args.M}_{args.K}',
                          [x_in], [y_out], [W_init])

# Bump opset to a recent one supported by qairt-converter / QNN
model = helper.make_model(graph,
                          opset_imports=[helper.make_opsetid('', 17)],
                          ir_version=8)
onnx.checker.check_model(model)
onnx.save(model, args.out)
print(f"wrote {args.out}: input x[1,{args.K}], output y[1,{args.M}], "
      f"weight {args.K}x{args.M} FP16 ({W.nbytes/1024/1024:.1f} MB)")
