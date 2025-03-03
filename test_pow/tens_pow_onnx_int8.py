#!/usr/bin/env python3
import onnxruntime as ort
import numpy as np
import sys
import time
import hashlib
from typing import Tuple, Optional

# Set OPS_PER_HASH as in the C code (operations per hash)
OPS_PER_HASH = 256 * 256 * 64 + 32 * 256 * 2  # 4,210,688

# Use the INT8 model file (which now expects UINT8 inputs/outputs)
MODEL_PATH = "tens_hash_int8.onnx"
try:
    session_options = ort.SessionOptions()
    session_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    session_options.intra_op_num_threads = 8  # Utilize multiple cores

    # Try CoreML first for Apple Silicon
    providers = ["CoreMLExecutionProvider", "CPUExecutionProvider"]
    session = ort.InferenceSession(MODEL_PATH, providers=providers, sess_options=session_options)
    print("Using CoreML acceleration")
    print("Active execution providers:", session.get_providers())
except Exception as e:
    print(f"CoreML not available ({str(e)}), falling back to CPU")
    session = ort.InferenceSession(
        MODEL_PATH,
        providers=["CPUExecutionProvider"],
        sess_options=session_options
    )

def nonce_to_input(nonce: int) -> np.ndarray:
    """
    Convert a single nonce to input array format of shape [1, 32] as uint8.
    The nonce is converted to a 32-byte big-endian representation.
    """
    nonce_bytes = nonce.to_bytes(32, byteorder='big')
    return np.frombuffer(nonce_bytes, dtype=np.uint8).reshape(1, 32)

def compute_error(input_array: np.ndarray) -> np.ndarray:
    """
    Compute error vector for a single input using SHA-256.
    Returns a uint8 array with shape [1, 32].
    """
    input_bytes = input_array.tobytes()  # input_array is already uint8
    digest = hashlib.sha256(input_bytes).digest()
    return np.frombuffer(digest[:32], dtype=np.uint8).reshape(1, 32)

def run_model_single(nonce: int) -> np.ndarray:
    """
    Run the ONNX model on a single nonce.
    Returns the model output as a uint8 array.
    """
    input_array = nonce_to_input(nonce)
    error = compute_error(input_array)
    output = session.run(None, {"input": input_array, "error": error})[0]
    return output  # output is uint8

def count_leading_zero_bits(arr: bytes) -> int:
    """Count leading zero bits in a byte array."""
    bits = ''.join(bin(byte)[2:].rjust(8, '0') for byte in arr)
    return len(bits) - len(bits.lstrip('0'))

def search_pow(target_bits: int, max_nonce: Optional[int] = None) -> Tuple[int, bytes]:
    """Search for a nonce that produces the required number of leading zero bits."""
    print(f"Difficulty: {target_bits} leading 0 bits")
    print("Progress:")
    print("  Time    Hash Rate      TOPS         Total Hashes    Best Bits")
    print("  ----    ---------      --------     ------------    ----------")

    nonce = 0
    start_time = time.time()
    hashes = 0
    last_report = start_time
    last_hashes = 0
    best_bits = 0

    while True:
        if max_nonce and nonce >= max_nonce:
            raise ValueError(f"Failed to find solution with {target_bits} leading zero bits after {nonce} attempts")

        output = run_model_single(nonce)
        output_bytes = output.tobytes()  # already uint8
        bits = count_leading_zero_bits(output_bytes)
        best_bits = max(best_bits, bits)
        hashes += 1

        if bits >= target_bits:
            elapsed = time.time() - start_time
            avg_hash_rate = hashes / elapsed
            avg_tops = (avg_hash_rate * OPS_PER_HASH) / 1e12
            print()  # New line after progress updates
            return nonce, output_bytes

        nonce += 1

        now = time.time()
        if now - last_report >= 1.0:
            interval = now - last_report
            interval_hashes = hashes - last_hashes
            hash_rate = interval_hashes / interval
            tops = (hash_rate * OPS_PER_HASH) / 1e12
            total_time = now - start_time
            print(f"\r  {total_time:4.0f}s    {hash_rate:9.0f} h/s    {tops:10.6f}    {hashes:12}    {best_bits:10}", end="", flush=True)
            last_report = now
            last_hashes = hashes

def main():
    if len(sys.argv) != 2:
        sys.exit("Usage: python tens_pow_onnx_int8.py <target_bits>")

    try:
        target_bits = int(sys.argv[1])
        if target_bits < 0 or target_bits > 256:
            raise ValueError("Target bits must be between 0 and 256")
    except ValueError as e:
        sys.exit(f"Error: {str(e)}")

    try:
        nonce, output = search_pow(target_bits)
        print(f"\nSuccess! Found nonce: {nonce}")
        print(f"Output hash: {output.hex()}")
        print(f"Leading zero bits: {count_leading_zero_bits(output)}")
    except ValueError as e:
        sys.exit(f"Error: {str(e)}")
    except KeyboardInterrupt:
        print("\nSearch interrupted by user")
        sys.exit(1)

if __name__ == "__main__":
    main()
