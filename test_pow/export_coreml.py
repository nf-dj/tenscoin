#!/usr/bin/env python3
import sys
import numpy as np
from Crypto.Cipher import ChaCha20
import coremltools as ct
from coremltools.models.neural_network import NeuralNetworkBuilder
from coremltools.models import datatypes

ROUNDS=64

def generate_ternary_matrix_from_seed(seed):
    input_size, output_size = 256, 256
    A = np.zeros((input_size, output_size), dtype=np.float32)
    pos_count = neg_count = 32

    for i in range(input_size):
        nonce = i.to_bytes(8, 'big')
        cipher = ChaCha20.new(key=seed, nonce=nonce)
        
        rand_bytes = cipher.encrypt(b'\x00' * (output_size * 4))
        rand_ints = np.frombuffer(rand_bytes, dtype=np.int32)
        chosen_indices = np.argsort(rand_ints)[:64]
        
        rand_bytes_shuffle = cipher.encrypt(b'\x00' * (64 * 4))
        shuffle_ints = np.frombuffer(rand_bytes_shuffle, dtype=np.int32)
        shuffle_perm = np.argsort(shuffle_ints)
        sign_vector = np.array([1] * pos_count + [-1] * neg_count, dtype=np.float32)
        sign_vector = sign_vector[shuffle_perm]
        
        A[i, chosen_indices] = sign_vector
    return A

def create_model(ternary_matrix):
    input_features = [
        ('input', datatypes.Array(1, 256)),
        ('noise', datatypes.Array(1, 256))
    ]
    output_features = [('output', datatypes.Array(1, 256))]
    
    builder = NeuralNetworkBuilder(
        input_features,
        output_features,
        disable_rank5_shape_mapping=True
    )

    previous_output = "input"
    
    # Two rounds
    for i in range(ROUNDS):
        # Matrix multiply
        matmul_name = f"matmul_{i}"
        builder.add_inner_product(
            name=matmul_name,
            input_name=previous_output,
            output_name=f"{matmul_name}_out",
            input_channels=256,
            output_channels=256,
            W=ternary_matrix.astype(np.float32),
            b=None,
            has_bias=False
        )

        # Add noise
        noise_add_name = f"noise_add_{i}"
        builder.add_add_broadcastable(
            name=noise_add_name,
            input_names=[f"{matmul_name}_out", "noise"],
            output_name=f"{noise_add_name}_out"
        )

        # Clip to [0,1]
        clip_name = f"clip_{i}"
        output_name = f"round_{i}_out" if i < ROUNDS-1 else "output"
        builder.add_clip(
            name=clip_name,
            input_name=f"{noise_add_name}_out",
            output_name=output_name,
            min_value=0.0,
            max_value=1.0
        )

        previous_output = output_name

    return builder

def main():
    if len(sys.argv) != 2:
        print("Usage: export_coreml.py <32-byte-hex-seed>")
        sys.exit(1)
        
    seed_hex = sys.argv[1]
    if len(seed_hex) != 64:
        print("Error: Seed must be 32 bytes (64 hex chars)")
        sys.exit(1)

    try:
        print("Generating ternary matrix...")
        seed = bytes.fromhex(seed_hex)
        ternary_matrix = generate_ternary_matrix_from_seed(seed)
        
        print("Creating model architecture...")
        builder = create_model(ternary_matrix)
        
        print("Creating model...")
        spec = builder.spec
        model = ct.models.MLModel(spec)
        
        print("Saving model...")
        model.save("tens_hash.mlmodel")
        
        print("Compiling model...")
        import os
        os.system(f"xcrun coremlc compile tens_hash.mlmodel .")
        print("Model compiled successfully!")
        
    except Exception as e:
        print(f"Error creating model: {str(e)}")
        import traceback
        traceback.print_exc()

if __name__ == "__main__":
    main()
