#!/usr/bin/env python3
import argparse
import time
from typing import List, Tuple
import numpy as np
from Crypto.Cipher import ChaCha20
import torch
import torch.nn as nn

# Network dimensions
INPUT_SIZE = 256      # Input vector size 
HIDDEN_SIZE = 1024    # Hidden layer size
NUM_HIDDEN_LAYERS = 64
BATCH_SIZE = 2048     # Batch size for inference
NUM_NONZERO = 128

class HashNetwork(nn.Module):
    def __init__(self, matrices):
        super().__init__()
        self.expansion = matrices[0][1]  # Already a tensor in FP16
        self.hidden_layers = [matrices[i+1][1] for i in range(NUM_HIDDEN_LAYERS)]
        self.compression = matrices[-1][1]  # Already a tensor in FP16

    def to(self, device):
        self.expansion = self.expansion.to(device)
        self.hidden_layers = [layer.to(device) for layer in self.hidden_layers]
        self.compression = self.compression.to(device)
        return self

    def forward(self, x):  # x shape: (INPUT_SIZE, BATCH_SIZE)
        # First expansion layer
        x = torch.matmul(self.expansion, (2.0 * x - 1.0))  # Fused operations
        x = torch.clip(x, 0.0, 1.0)
        
        # Hidden layers with residual connections
        for layer in self.hidden_layers:
            x_mapped = 2.0 * x - 1.0
            x = torch.clip(torch.matmul(layer, x_mapped) + x_mapped, 0.0, 1.0)  # Fused operations
        
        # Compression layer
        x = torch.clip(torch.matmul(self.compression, (2.0 * x - 1.0)), 0.0, 1.0)  # Fused operations
        
        return x

def hex_to_bytes(hex_str):
    """Convert hex string to bytes."""
    b = bytes.fromhex(hex_str)
    if len(b) != 32:
        raise ValueError("Seed must be 32 bytes (64 hex characters)")
    return b

def generate_dense_matrix(rows, cols, key, nonce_int):
    """Generate a constant matrix using ChaCha20-based RNG."""
    nonce = nonce_int.to_bytes(8, byteorder='big')
    cipher = ChaCha20.new(key=key, nonce=nonce)
    
    needed = rows * cols
    random_bytes = cipher.encrypt(b'\x00' * needed)
    data = np.frombuffer(random_bytes, dtype=np.uint8)
    
    mods = data % 4
    mapping = np.empty_like(mods, dtype=np.int8)
    mapping[mods == 0] = 0
    mapping[mods == 1] = 0
    mapping[mods == 2] = 1
    mapping[mods == 3] = -1
    
    # Create tensor directly in FP16
    return torch.from_numpy(mapping.reshape((rows, cols))).half()

def generate_model(seed: bytes) -> HashNetwork:
    """Generate the model."""
    print("Generating model...")
    layers = []
    nonce_counter = 0
    
    # Expansion layer
    expansion_mat = generate_dense_matrix(HIDDEN_SIZE, INPUT_SIZE, seed, nonce_counter)
    nonce_counter += 1
    layers.append(('expansion', expansion_mat))
    
    # Hidden layers
    for i in range(NUM_HIDDEN_LAYERS):
        mat_hidden = generate_dense_matrix(HIDDEN_SIZE, HIDDEN_SIZE, seed, nonce_counter)
        nonce_counter += 1
        layers.append(('hidden', mat_hidden))
    
    # Compression layer
    compression_mat = generate_dense_matrix(INPUT_SIZE, HIDDEN_SIZE, seed, nonce_counter)
    nonce_counter += 1
    layers.append(('compression', compression_mat))
    
    print(f"Generated layers in {layers[0][1].dtype} precision")
    
    return HashNetwork(layers)

class ProofOfWorkMiner:
    def __init__(self, model: HashNetwork, target_difficulty: int):
        self.target_difficulty = target_difficulty
        
        # Use MPS (Metal) if available
        if torch.backends.mps.is_available():
            print("Using MPS (Metal) device with half precision")
            self.device = torch.device("mps")
        elif torch.cuda.is_available():
            print("Using CUDA device with half precision")
            self.device = torch.device("cuda")
        else:
            print("Using CPU device")
            self.device = torch.device("cpu")
            
        # Move model to device and convert to half precision
        self.model = model.to(self.device)
        self.model.eval()
        
        # Initialize tracking variables
        self.nonce = 0
        self.total_hashes = 0
        self.start_time = time.time()
        self.best_difficulty = 0
        
        # Pre-allocate arrays for batch processing
        self.nonce_bytes = np.zeros((BATCH_SIZE, 32), dtype=np.uint8)
        self.binary_input = torch.zeros((INPUT_SIZE, BATCH_SIZE), 
                                      dtype=torch.float16, 
                                      device=self.device)

    def prepare_batch(self) -> None:
        # Generate sequential nonces
        nonces = np.arange(self.nonce, self.nonce + BATCH_SIZE, dtype=np.uint64)
        for i in range(8):
            self.nonce_bytes[:, 24 + i] = (nonces >> (8 * (7 - i))) & 0xFF
            
        # Convert to binary
        bits = np.unpackbits(self.nonce_bytes, axis=1)[:, :INPUT_SIZE].T.astype(np.float32)
        self.binary_input.copy_(torch.from_numpy(bits).half())

    def count_leading_zeros(self, output: torch.Tensor) -> int:
        """Count leading zeros in binary output."""
        binary = (output.cpu() > 0.5).numpy()
        nonzero_indices = np.nonzero(binary)[0]
        return nonzero_indices[0] if len(nonzero_indices) > 0 else INPUT_SIZE

    def print_status(self):
        """Print current mining status."""
        elapsed = time.time() - self.start_time
        hashrate = self.total_hashes / elapsed
        tops = (hashrate * (NUM_HIDDEN_LAYERS * HIDDEN_SIZE * HIDDEN_SIZE * 2 + HIDDEN_SIZE * INPUT_SIZE * 4)) / 1e12
        print(f"Nonce: {self.nonce} | Hashrate: {hashrate:.2f} H/s | "
              f"TOPS: {tops:.2f} | Best difficulty: {self.best_difficulty}")

    def check_solution(self, binary_output: torch.Tensor, batch_index: int) -> bool:
        """Check if a solution meets the target difficulty."""
        column = binary_output[:, batch_index]
        zeros = self.count_leading_zeros(column)
        self.best_difficulty = max(self.best_difficulty, zeros)
        
        if zeros >= self.target_difficulty:
            solution_nonce = self.nonce + batch_index
            print("\nSolution found!")
            print(f"Nonce: {solution_nonce}")
            print(f"Leading zeros: {zeros}")
            print(f"Solution input (hex): {bytes(self.nonce_bytes[batch_index]).hex()}")
            
            # Ensure correct output format
            output_bits = (column.cpu().numpy() > 0.5).astype(np.uint8)
            output_bytes = np.packbits(output_bits)
            hex_output = bytes(output_bytes).hex()
            print(f"Model output (hex): {hex_output}")
            return True
        return False

    @torch.no_grad()
    def mine(self):
        """Main mining loop."""
        print(f"Mining with difficulty: {self.target_difficulty}")
        last_status_time = time.time()
        
        while True:
            self.prepare_batch()
            binary_output = self.model(self.binary_input)
            
            for i in range(BATCH_SIZE):
                if self.check_solution(binary_output, i):
                    return
            
            self.nonce += BATCH_SIZE
            self.total_hashes += BATCH_SIZE
            
            if time.time() - last_status_time >= 1.0:
                self.print_status()
                last_status_time = time.time()

def main():
    parser = argparse.ArgumentParser(description="PyTorch Proof of Work on Metal")
    parser.add_argument("seed", type=str, help="32-byte hex seed (64 hex characters)")
    parser.add_argument("difficulty", type=int, help="Target difficulty (number of leading zeros)")
    args = parser.parse_args()
    
    # Generate model from seed
    seed = hex_to_bytes(args.seed)
    model = generate_model(seed)
    
    # Start mining
    miner = ProofOfWorkMiner(model, args.difficulty)
    miner.mine()

if __name__ == "__main__":
    main()
