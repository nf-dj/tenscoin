// Copyright (c) 2025-present The Tenscoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://opensource.org/licenses/mit-license.php.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sodium.h>
#include <ctype.h>  // for isxdigit()

// -----------------------------------------------------------------------------
// Definitions
// -----------------------------------------------------------------------------
#define IN_SIZE 32
#define HIDDEN 1024
#define ROUNDS 64

// -----------------------------------------------------------------------------
// Data Structures for Precomputation
// -----------------------------------------------------------------------------
typedef struct {
    uint8_t **expand_mat;         // HIDDEN x IN_SIZE
    uint8_t **middle_mats[ROUNDS]; // ROUNDS of HIDDEN x HIDDEN
    uint8_t **compress_mat;       // IN_SIZE x HIDDEN
} PrecomputedMatrices;

typedef struct {
    uint8_t *state;
    uint8_t *next_state;
    int8_t  *noise;  // noise buffer of size: HIDDEN + (ROUNDS * HIDDEN) + IN_SIZE
} HashBuffers;

// -----------------------------------------------------------------------------
// Function Prototypes (added to fix implicit declarations)
// -----------------------------------------------------------------------------
PrecomputedMatrices* precompute_matrices(uint8_t seed[32]);
void free_matrices(PrecomputedMatrices* matrices);
HashBuffers* init_hash_buffers(void);
void free_hash_buffers(HashBuffers* buffers);
void tens_hash_precomputed(uint8_t input[IN_SIZE],
                           PrecomputedMatrices* matrices,
                           HashBuffers* buffers,
                           uint8_t output[IN_SIZE]);

// -----------------------------------------------------------------------------
// Matrix multiplication: computes (A * in + noise) mod 256.
// -----------------------------------------------------------------------------
static void matrix_multiply(uint8_t **A, uint8_t *in, uint8_t *out, int8_t *e, int rows, int cols) {
    for (int i = 0; i < rows; i++) {
        int32_t sum = 0;
        for (int j = 0; j < cols; j++) {
            sum += (int32_t)A[i][j] * in[j];
        }
        sum += e[i];
        out[i] = ((uint8_t)sum) & 0xFF;
    }
}

// -----------------------------------------------------------------------------
// generate_matrices(): fills the provided matrices using ChaCha20.
// Uses a fixed, all-zero nonce.
// (This function is not changed by our update.)
// -----------------------------------------------------------------------------
static void generate_matrices(uint8_t **expand_mat,
                              uint8_t **middle_mats[ROUNDS],
                              uint8_t **compress_mat,
                              uint8_t seed[32]) {
    size_t total_size = (HIDDEN * IN_SIZE) + (ROUNDS * HIDDEN * HIDDEN) + (IN_SIZE * HIDDEN);
    uint8_t *data = malloc(total_size);
    if (!data) {
        fprintf(stderr, "Memory allocation error in generate_matrices\n");
        exit(1);
    }
    unsigned char nonce[crypto_stream_chacha20_NONCEBYTES] = {0}; // fixed nonce
    crypto_stream_chacha20(data, total_size, nonce, seed);

    uint8_t *pos = data;
    memcpy(expand_mat[0], pos, HIDDEN * IN_SIZE);
    pos += HIDDEN * IN_SIZE;

    for (int r = 0; r < ROUNDS; r++) {
        memcpy(middle_mats[r][0], pos, HIDDEN * HIDDEN);
        pos += HIDDEN * HIDDEN;
    }

    memcpy(compress_mat[0], pos, IN_SIZE * HIDDEN);
    free(data);
}

// -----------------------------------------------------------------------------
// tens_hash_precomputed(): computes the hash using precomputed matrices and
// pre-allocated buffers.
// -----------------------------------------------------------------------------
void tens_hash_precomputed(uint8_t input[IN_SIZE],
                           PrecomputedMatrices* matrices,
                           HashBuffers* buffers,
                           uint8_t output[IN_SIZE]) {
    int total_noise = HIDDEN + (ROUNDS * HIDDEN) + IN_SIZE;
    // Instead of using ChaCha20, compute SHA-256 of the input.
    // This produces a 32-byte digest, which we then "wrap" (repeat) over the entire noise buffer.
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, input, IN_SIZE);
    for (int i = 0; i < total_noise; i++) {
        buffers->noise[i] = digest[i % crypto_hash_sha256_BYTES];
    }

    int8_t *expand_noise = buffers->noise;
    int8_t *middle_noise = buffers->noise + HIDDEN;
    int8_t *compress_noise = buffers->noise + HIDDEN + (ROUNDS * HIDDEN);

    // Expansion step
    matrix_multiply(matrices->expand_mat, input, buffers->state, expand_noise, HIDDEN, IN_SIZE);

    // Middle rounds: use pre-allocated state buffers and swap after each round.
    for (uint32_t round = 0; round < ROUNDS; round++) {
        matrix_multiply(matrices->middle_mats[round], buffers->state, buffers->next_state, middle_noise + (round * HIDDEN), HIDDEN, HIDDEN);
        uint8_t *temp = buffers->state;
        buffers->state = buffers->next_state;
        buffers->next_state = temp;
    }

    // Compression step
    matrix_multiply(matrices->compress_mat, buffers->state, output, compress_noise, IN_SIZE, HIDDEN);
}

// -----------------------------------------------------------------------------
// tens_hash(): computes the hash using the precomputed method to avoid code duplication.
// It precomputes the matrices and buffers for the given seed, calls tens_hash_precomputed(),
// then frees the precomputed data.
// -----------------------------------------------------------------------------
void tens_hash(uint8_t input[IN_SIZE],
               uint8_t seed[32],
               uint8_t output[IN_SIZE]) {
    PrecomputedMatrices* matrices = precompute_matrices(seed);
    if (!matrices) {
        fprintf(stderr, "Failed to precompute matrices.\n");
        exit(1);
    }
    HashBuffers* buffers = init_hash_buffers();
    if (!buffers) {
        free_matrices(matrices);
        fprintf(stderr, "Failed to initialize hash buffers.\n");
        exit(1);
    }
    tens_hash_precomputed(input, matrices, buffers, output);
    free_matrices(matrices);
    free_hash_buffers(buffers);
}

// ==============================================================================
// Precomputation and Precomputed Hash Functions
// (These functions are used by tens_pow.c.)
// ==============================================================================

// precompute_matrices(): allocates and fills the matrices from the given seed.
PrecomputedMatrices* precompute_matrices(uint8_t seed[32]) {
    PrecomputedMatrices* matrices = malloc(sizeof(PrecomputedMatrices));
    if (!matrices) return NULL;

    // Allocate expansion matrix
    matrices->expand_mat = malloc(HIDDEN * sizeof(uint8_t*));
    if (!matrices->expand_mat) { free(matrices); return NULL; }
    matrices->expand_mat[0] = malloc(HIDDEN * IN_SIZE);
    if (!matrices->expand_mat[0]) { free(matrices->expand_mat); free(matrices); return NULL; }
    for (int i = 1; i < HIDDEN; i++) {
        matrices->expand_mat[i] = matrices->expand_mat[0] + (i * IN_SIZE);
    }

    // Allocate middle matrices for each round
    for (int r = 0; r < ROUNDS; r++) {
        matrices->middle_mats[r] = malloc(HIDDEN * sizeof(uint8_t*));
        if (!matrices->middle_mats[r]) {
            for (int j = 0; j < r; j++) {
                free(matrices->middle_mats[j][0]);
                free(matrices->middle_mats[j]);
            }
            free(matrices->expand_mat[0]);
            free(matrices->expand_mat);
            free(matrices);
            return NULL;
        }
        matrices->middle_mats[r][0] = malloc(HIDDEN * HIDDEN);
        if (!matrices->middle_mats[r][0]) {
            for (int j = 0; j < r; j++) {
                free(matrices->middle_mats[j][0]);
                free(matrices->middle_mats[j]);
            }
            free(matrices->middle_mats[r]);
            free(matrices->expand_mat[0]);
            free(matrices->expand_mat);
            free(matrices);
            return NULL;
        }
        for (int i = 1; i < HIDDEN; i++) {
            matrices->middle_mats[r][i] = matrices->middle_mats[r][0] + (i * HIDDEN);
        }
    }

    // Allocate compression matrix
    matrices->compress_mat = malloc(IN_SIZE * sizeof(uint8_t*));
    if (!matrices->compress_mat) {
        for (int r = 0; r < ROUNDS; r++) {
            free(matrices->middle_mats[r][0]);
            free(matrices->middle_mats[r]);
        }
        free(matrices->expand_mat[0]);
        free(matrices->expand_mat);
        free(matrices);
        return NULL;
    }
    matrices->compress_mat[0] = malloc(IN_SIZE * HIDDEN);
    if (!matrices->compress_mat[0]) {
        for (int r = 0; r < ROUNDS; r++) {
            free(matrices->middle_mats[r][0]);
            free(matrices->middle_mats[r]);
        }
        free(matrices->compress_mat);
        free(matrices->expand_mat[0]);
        free(matrices->expand_mat);
        free(matrices);
        return NULL;
    }
    for (int i = 1; i < IN_SIZE; i++) {
        matrices->compress_mat[i] = matrices->compress_mat[0] + (i * HIDDEN);
    }

    // Fill matrices using the given seed.
    generate_matrices(matrices->expand_mat, matrices->middle_mats, matrices->compress_mat, seed);
    return matrices;
}

// free_matrices(): frees all memory allocated in the PrecomputedMatrices structure.
void free_matrices(PrecomputedMatrices* matrices) {
    if (matrices) {
        if (matrices->expand_mat) {
            if (matrices->expand_mat[0])
                free(matrices->expand_mat[0]);
            free(matrices->expand_mat);
        }
        for (int r = 0; r < ROUNDS; r++) {
            if (matrices->middle_mats[r]) {
                if (matrices->middle_mats[r][0])
                    free(matrices->middle_mats[r][0]);
                free(matrices->middle_mats[r]);
            }
        }
        if (matrices->compress_mat) {
            if (matrices->compress_mat[0])
                free(matrices->compress_mat[0]);
            free(matrices->compress_mat);
        }
        free(matrices);
    }
}

// init_hash_buffers(): allocates and initializes the hash buffers.
HashBuffers* init_hash_buffers(void) {
    HashBuffers* buffers = malloc(sizeof(HashBuffers));
    if (!buffers) return NULL;
    buffers->state = calloc(HIDDEN, sizeof(uint8_t));
    buffers->next_state = calloc(HIDDEN, sizeof(uint8_t));
    int total_noise = HIDDEN + (ROUNDS * HIDDEN) + IN_SIZE;
    buffers->noise = malloc(total_noise * sizeof(int8_t));
    if (!buffers->state || !buffers->next_state || !buffers->noise) {
        if (buffers->state) free(buffers->state);
        if (buffers->next_state) free(buffers->next_state);
        if (buffers->noise) free(buffers->noise);
        free(buffers);
        return NULL;
    }
    return buffers;
}

// free_hash_buffers(): frees the memory used by the hash buffers.
void free_hash_buffers(HashBuffers* buffers) {
    if (buffers) {
        if (buffers->state) free(buffers->state);
        if (buffers->next_state) free(buffers->next_state);
        if (buffers->noise) free(buffers->noise);
        free(buffers);
    }
}

// -----------------------------------------------------------------------------
// (Optional) Hex parsing utility functions
// -----------------------------------------------------------------------------

// hexchar_to_int(): converts a single hex character to its integer value.
int hexchar_to_int(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// parse_hex(): manually parses a hex string into a binary buffer.
// The hex string must be exactly out_len * 2 characters long.
int parse_hex(const char *hex, size_t hex_len, uint8_t *out, size_t out_len) {
    if (hex_len != out_len * 2)
        return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hexchar_to_int(hex[2 * i]);
        int lo = hexchar_to_int(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (hi << 4) | lo;
    }
    return 0;
}

#ifdef HASH_MAIN
// Main function for tens_hash (if compiled standalone).
// (This is used by the tens_hash target; tens_pow uses the precomputed functions.)
int main(int argc, char *argv[]) {
    if (sodium_init() < 0)
        return 1;
    if (argc < 3)
        return 1;
    if (strlen(argv[1]) != 64)
        return 1;
    uint8_t seed[32];
    if (parse_hex(argv[1], strlen(argv[1]), seed, sizeof(seed)) != 0)
        return 1;
    if (strlen(argv[2]) != 64)
        return 1;
    uint8_t input[IN_SIZE];
    if (parse_hex(argv[2], strlen(argv[2]), input, sizeof(input)) != 0)
        return 1;
    uint8_t output[IN_SIZE];
    tens_hash(input, seed, output);
    for (int i = 0; i < IN_SIZE; i++)
        printf("%02x", output[i]);
    printf("\n");
    return 0;
}
#endif

