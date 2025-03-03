#include "tens_hash.h"
#include <stdlib.h>
#include <string.h>
#include <crypto/chacha20.h>
#include <crypto/common.h>
#include <vector>
#include <span>
#include <logging.h>
#include <util/strencodings.h>

#define TENS_HIDDEN 1024             // Hidden layer size (number of neurons)
#define NUM_HIDDEN_LAYERS 64         // Number of hidden layers

// INPUT_BITS is the number of bits in the input vector.
#define INPUT_BITS (TENS_IN_SIZE * 8) // 256 bits

static uint64_t to_big_endian(uint64_t val) {
    return ((val & 0x00000000000000FFULL) << 56) |
           ((val & 0x000000000000FF00ULL) << 40) |
           ((val & 0x0000000000FF0000ULL) << 24) |
           ((val & 0x00000000FF000000ULL) << 8)  |
           ((val & 0x000000FF00000000ULL) >> 8)  |
           ((val & 0x0000FF0000000000ULL) >> 24) |
           ((val & 0x00FF000000000000ULL) >> 40) |
           ((val & 0xFF00000000000000ULL) >> 56);
}

static void generate_dense_matrix(int rows, int cols, const uint8_t* seed, uint64_t nonce_counter, int8_t* matrix)
{
    int total = rows * cols;
    std::vector<std::byte> keystream(total);
    std::vector<std::byte> key_bytes(32);
    std::memcpy(key_bytes.data(), seed, 32);
    Span<const std::byte> key_span(key_bytes);

    // Build a 96-bit nonce as required by Bitcoin’s ChaCha20.
    // Bitcoin’s ChaCha20::Nonce96 is defined as a std::pair<uint32_t, uint64_t>.
    // Here, we set the first 4 bytes to zero and the last 8 bytes to nonce_counter.
    uint64_t nonce_be = to_big_endian(nonce_counter);
    ChaCha20::Nonce96 nonce = std::make_pair(0u, nonce_be);

    // Create ChaCha20 instance and seek using block counter 0.
    ChaCha20 chacha(key_span);
    chacha.Seek(nonce, 0);
    Span<std::byte> keystream_span(keystream);
    chacha.Keystream(keystream_span);

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(keystream.data());
    for (int i = 0; i < total; i++) {
        uint8_t mod = bytes[i] % 4;
        int8_t val;
        if (mod == 0 || mod == 1)
            val = 0;
        else if (mod == 2)
            val = 1;
        else // mod == 3
            val = -1;
        matrix[i] = val;
    }
}

static void print_matrix(const int8_t* matrix, int num_to_print)
{
    printf("First %d elements of matrix: ", num_to_print);
    for (int i = 0; i < num_to_print; i++) {
         printf("%d ", matrix[i]);
    }
    printf("\n");
}

// Generate all matrices (expansion, hidden layers, and compression) using the seed.
static void generate_all_matrices(TensHashContext* ctx, const uint8_t seed[32])
{
    if (!ctx || !seed) return;
    uint64_t nonce_counter = 0;

    // Expansion matrix: dimensions: TENS_HIDDEN x INPUT_BITS (1024 x 256)
    generate_dense_matrix(TENS_HIDDEN, INPUT_BITS, seed, nonce_counter++, ctx->expansion_mat);
    //print_matrix(ctx->expansion_mat, 16);

    // Hidden matrices: NUM_HIDDEN_LAYERS matrices, each of size TENS_HIDDEN x TENS_HIDDEN (1024 x 1024)
    for (int r = 0; r < NUM_HIDDEN_LAYERS; r++) {
        generate_dense_matrix(TENS_HIDDEN, TENS_HIDDEN, seed, nonce_counter++,
                              ctx->hidden_mats + r * TENS_HIDDEN * TENS_HIDDEN);
        //print_matrix(ctx->hidden_mats + r * TENS_HIDDEN * TENS_HIDDEN, 16);
    }

    // Compression matrix: dimensions: INPUT_BITS x TENS_HIDDEN (256 x 1024)
    generate_dense_matrix(INPUT_BITS, TENS_HIDDEN, seed, nonce_counter++, ctx->compression_mat);
    //print_matrix(ctx->compression_mat, 16);
}

// Modified forward propagation for one layer.
//  1. Map input: x_mapped[i] = 2*input[i] - 1  (i.e., 0 → -1, 1 → +1)
//  2. For each output neuron, compute dot product with corresponding matrix row.
//  3. For layers where in_dim == out_dim (hidden layers), add the residual connection:
//     add the corresponding element of x_mapped.
//  4. Apply threshold: if sum > 0 then output = 1, else output = 0.
static void layer_forward(const int8_t* matrix, int in_dim, int out_dim, const int8_t* input, int8_t* output)
{
    int8_t* x_mapped = (int8_t*)malloc(in_dim * sizeof(int8_t));
    if (!x_mapped) {
        fprintf(stderr, "Memory allocation error in layer_forward\n");
        exit(1);
    }
    for (int i = 0; i < in_dim; i++) {
        x_mapped[i] = (int8_t)(2 * input[i] - 1);
    }
    for (int j = 0; j < out_dim; j++) {
        int32_t sum = 0;
        const int8_t* row = matrix + j * in_dim;
        for (int i = 0; i < in_dim; i++) {
            sum += row[i] * x_mapped[i];
        }
        // Add residual connection if dimensions match (hidden layers: 1024→1024)
        if (in_dim == out_dim) {
            sum += x_mapped[j];
        }
        output[j] = sum > 0 ? 1 : 0;
    }
    free(x_mapped);
}

// Pack 256 bits (stored as int8_t with nonzero = 1) into 32 bytes.
static void pack_bits(const int8_t* bits, uint8_t* out_bytes)
{
    memset(out_bytes, 0, TENS_IN_SIZE);
    for (int i = 0; i < INPUT_BITS; i++) {
        if (bits[i])
            out_bytes[i / 8] |= (1 << (7 - (i % 8)));
    }
}

// Allocate buffers inside the context.
static bool alloc_context_buffers(TensHashContext* ctx)
{
    if (!ctx) return false;
    // Expansion matrix: TENS_HIDDEN x INPUT_BITS
    ctx->expansion_mat = (int8_t*)malloc(TENS_HIDDEN * INPUT_BITS * sizeof(int8_t));
    // Hidden matrices: NUM_HIDDEN_LAYERS matrices each of size TENS_HIDDEN x TENS_HIDDEN
    ctx->hidden_mats = (int8_t*)malloc(NUM_HIDDEN_LAYERS * TENS_HIDDEN * TENS_HIDDEN * sizeof(int8_t));
    // Compression matrix: INPUT_BITS x TENS_HIDDEN
    ctx->compression_mat = (int8_t*)malloc(INPUT_BITS * TENS_HIDDEN * sizeof(int8_t));
    // Allocate state buffers (using TENS_HIDDEN as working size)
    ctx->state = (int8_t*)calloc(TENS_HIDDEN, sizeof(int8_t));
    ctx->next_state = (int8_t*)calloc(TENS_HIDDEN, sizeof(int8_t));

    if (!ctx->expansion_mat || !ctx->hidden_mats || !ctx->compression_mat || !ctx->state || !ctx->next_state) {
        return false;
    }
    return true;
}

TensHashContext* tens_hash_init(const uint8_t seed[32])
{
    if (!seed) return nullptr;
    TensHashContext* ctx = (TensHashContext*)malloc(sizeof(TensHashContext));
    if (!ctx) return nullptr;
    memset(ctx, 0, sizeof(TensHashContext));
    if (!alloc_context_buffers(ctx)) {
        tens_hash_free(ctx);
        return nullptr;
    }

    // Swap the seed bytes from LSB-first to MSB-first.
    uint8_t swapped_seed[32];
    for (int i = 0; i < 32; i++) {
        swapped_seed[i] = seed[31 - i];
    }

    printf("tens_hash_init Seed (hex): ");
    for (int i = 0; i < TENS_IN_SIZE; i++) {
        printf("%02X", swapped_seed[i]);
    }
    printf("\n");

    // Use the swapped seed for generating matrices.
    generate_all_matrices(ctx, swapped_seed);
    return ctx;
}

void tens_hash_free(TensHashContext* ctx)
{
    if (ctx) {
        free(ctx->expansion_mat);
        free(ctx->hidden_mats);
        free(ctx->compression_mat);
        free(ctx->state);
        free(ctx->next_state);
        free(ctx);
    }
}

// Precomputed hash: process the input using the matrices in the context.
//   1. Convert the 32-byte input into 256 bits.
//   2. Apply expansion layer (256→1024).
//   3. Apply NUM_HIDDEN_LAYERS hidden layers (each 1024→1024) with residual connections.
//   4. Apply compression layer (1024→256).
//   5. Pack final 256 bits into 32 bytes.
void tens_hash_precomputed(const uint8_t input[TENS_IN_SIZE], TensHashContext* ctx, uint8_t output[TENS_IN_SIZE])
{
    if (!input || !ctx || !output) return;

    // Swap input: reverse the order (LSB-first -> MSB-first).
    uint8_t input_swapped[TENS_IN_SIZE];
    for (int i = 0; i < TENS_IN_SIZE; i++) {
        input_swapped[i] = input[TENS_IN_SIZE - 1 - i];
    }

    printf("tens_hash_precomputed Input: ");
    for (int i = 0; i < TENS_IN_SIZE; i++) {
        printf("%02X", input_swapped[i]);
    }
    printf("\n");

    // Convert swapped input bytes to 256 bits.
    memset(ctx->state, 0, TENS_HIDDEN); // clear working state
    for (int i = 0; i < TENS_IN_SIZE; i++) {
        for (int j = 0; j < 8; j++) {
            ctx->state[i * 8 + j] = (input_swapped[i] >> (7 - j)) & 1;
        }
    }

    // --- Expansion layer: from INPUT_BITS (256) to TENS_HIDDEN (1024) ---
    layer_forward(ctx->expansion_mat, INPUT_BITS, TENS_HIDDEN, ctx->state, ctx->next_state);
    int8_t* temp = ctx->state;
    ctx->state = ctx->next_state;
    ctx->next_state = temp;

    // --- Hidden layers: NUM_HIDDEN_LAYERS rounds (each 1024→1024) with residual connections ---
    for (int r = 0; r < NUM_HIDDEN_LAYERS; r++) {
        int8_t* matrix = ctx->hidden_mats + r * TENS_HIDDEN * TENS_HIDDEN;
        layer_forward(matrix, TENS_HIDDEN, TENS_HIDDEN, ctx->state, ctx->next_state);
        temp = ctx->state;
        ctx->state = ctx->next_state;
        ctx->next_state = temp;
    }

    // --- Compression layer: from TENS_HIDDEN (1024) to INPUT_BITS (256) ---
    layer_forward(ctx->compression_mat, TENS_HIDDEN, INPUT_BITS, ctx->state, ctx->next_state);

    // Final 256-element vector is in ctx->next_state.
    int8_t final_state[INPUT_BITS];
    memcpy(final_state, ctx->next_state, INPUT_BITS * sizeof(int8_t));

    // Pack the 256 bits into 32 bytes.
    pack_bits(final_state, output);

    uint8_t output_swapped[TENS_IN_SIZE];
    for (int i = 0; i < TENS_IN_SIZE; i++) {
        output_swapped[i] = output[TENS_IN_SIZE - 1 - i];
    }
    memcpy(output, output_swapped, TENS_IN_SIZE);

    printf("tens_hash_precomputed Output: ");
    for (int i = 0; i < TENS_IN_SIZE; i++) {
        printf("%02X", output[TENS_IN_SIZE - 1 - i]);
    }
    printf("\n");
}

void tens_hash(const uint8_t input[TENS_IN_SIZE], const uint8_t seed[32], uint8_t output[TENS_IN_SIZE])
{
    if (!input || !seed || !output) return;

    TensHashContext* ctx = tens_hash_init(seed);
    if (!ctx) return;
    tens_hash_precomputed(input, ctx, output);
    tens_hash_free(ctx);
}
