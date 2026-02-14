#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include <raylib.h>

#define WIDTH  64
#define HEIGHT 32
#define SCALE  16
#define FPS    60
#define CPU_HZ 500

#define MEMORY_SIZE 4096
#define STACK_SIZE  16
#define NUM_REG     16
#define NUM_KEY     16

static uint8_t memory[MEMORY_SIZE];
static uint8_t V[NUM_REG];
static uint16_t I;
static uint16_t pc;

static uint16_t stack[STACK_SIZE];
static uint8_t sp;

static uint8_t delay_timer;
static uint8_t sound_timer;

static bool display[HEIGHT][WIDTH];
static uint8_t keypad[NUM_KEY];

/* Quirks for compatibility */
static bool uses_vy;
static bool new_jump;
static bool modify_I;

static void chip8_init() {
    memset(memory, 0, MEMORY_SIZE);
    memset(V, 0, NUM_REG);
    memset(stack, 0, sizeof(stack));
    memset(display, 0, sizeof(display));
    memset(keypad, 0, sizeof(keypad));

    I  = 0;
    pc = 0x200;
    sp = 0;

    delay_timer = 0;
    sound_timer = 0;

    uses_vy  = false;
    new_jump = false;
    modify_I = false;

    SetRandomSeed(time(NULL));
}

static inline void clear_screen(void) {
    memset(display, 0, sizeof(display));
}

static void draw_screen(void) {
    for (size_t y = 0; y < HEIGHT; ++y) {
        for (size_t x = 0; x < WIDTH; ++x) {
            if (display[y][x]) {
                DrawRectangle(
                    x * SCALE,
                    y * SCALE,
                    SCALE,
                    SCALE,
                    WHITE
                );
            }
        }
    }
}

/*
 ** Standard font used for CHIP-8.
 */
#define FONTSET_SIZE 80
static
const uint8_t chip8_fontset[FONTSET_SIZE] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
    0x20, 0x60, 0x20, 0x20, 0x70, // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
    0x90, 0x90, 0xF0, 0x10, 0x10, // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
    0xF0, 0x10, 0x20, 0x40, 0x40, // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90, // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
    0xF0, 0x80, 0x80, 0x80, 0xF0, // C
    0xE0, 0x90, 0x90, 0x90, 0xE0, // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
    0xF0, 0x80, 0xF0, 0x80, 0x80  // F
};
#define FONTSET_START_ADDRESS 0x50

static inline void keypad_state(void) {
    keypad[0x0] = IsKeyDown(KEY_X);
    keypad[0x1] = IsKeyDown(KEY_ONE);
    keypad[0x2] = IsKeyDown(KEY_TWO);
    keypad[0x3] = IsKeyDown(KEY_THREE);
    keypad[0x4] = IsKeyDown(KEY_Q);
    keypad[0x5] = IsKeyDown(KEY_W);
    keypad[0x6] = IsKeyDown(KEY_E);
    keypad[0x7] = IsKeyDown(KEY_A);
    keypad[0x8] = IsKeyDown(KEY_S);
    keypad[0x9] = IsKeyDown(KEY_D);
    keypad[0xA] = IsKeyDown(KEY_Z);
    keypad[0xB] = IsKeyDown(KEY_C);
    keypad[0xC] = IsKeyDown(KEY_FOUR);
    keypad[0xD] = IsKeyDown(KEY_R);
    keypad[0xE] = IsKeyDown(KEY_F);
    keypad[0xF] = IsKeyDown(KEY_V);
}

/*
 ** Read and Write from memory operations
 */
static inline uint8_t read(uint16_t addr) {
    return memory[addr];
}

static inline bool write(uint16_t addr, uint8_t value) {
    /* Error: no space to write! */
    if (addr >= MEMORY_SIZE) return false;

    memory[addr] = value;
    return true;
}

static bool write_block(uint16_t start,
    const uint8_t * data, size_t size) {
    /* Error: no space to write! */
    if (start >= MEMORY_SIZE || size >= (size_t)(MEMORY_SIZE - start)) return false;

    for (size_t i = 0; i < size; ++i) {
        write(start + i, data[i]);
    }
    return true;
}

/*
 ** In CHIP-8, the source is typically loaded from address 0x50.
 */
static bool load_fontset(void) {
    return write_block(
        FONTSET_START_ADDRESS,
        chip8_fontset,
        FONTSET_SIZE
    );
}

/*
 ** Stack operations
 */
static inline void stack_push(uint16_t instruction) {
    if (sp >= STACK_SIZE) {
        fprintf(stderr, "Stack overflow (sp=%u)\n", sp);
        fprintf(stderr, "Stack overflow at PC=0x%X\n", pc);
        exit(1);
    }

    stack[sp++] = instruction;
}

static inline uint16_t stack_pop() {
    if (sp == 0) {
        fprintf(stderr, "Stack underflow (sp=%u)\n", sp);
        fprintf(stderr, "Stack underflow at PC=0x%X\n", pc);
        exit(1);
    }

    return stack[--sp];
}

static inline uint16_t fetch(void) {
    if (pc + 1 >= MEMORY_SIZE) {
        fprintf(stderr, "PC out of bounds in fetch: 0x%X\n", pc);
        exit(1);
    }
    uint16_t current_instruction = (memory[pc] << 8) | memory[pc + 1];
    pc += 2;
    return current_instruction;
}

static void decode_and_execute(void) {
    uint16_t opcode = fetch();

    /*
     ** X: The second nibble.
     ** Used to look up one of the 16 registers (VX) from V0 through VF.
     */
    uint8_t X = (opcode & 0x0F00) >> 8;
    /*
     ** Y: The third nibble.
     ** Also used to look up one of the 16 registers (VY) from V0 through VF.
     */
    uint8_t Y = (opcode & 0x00F0) >> 4;
    /*
     ** N: The fourth nibble. A 4-bit number.
     */
    uint8_t N = opcode & 0x000F;
    /*
     ** NN: The second byte (third and fourth nibbles). An 8-bit immediate number.
     */
    uint8_t NN = opcode & 0x00FF;
    /*
     ** NNN: The second, third and fourth nibbles. A 12-bit immediate memory address.
     */
    uint16_t NNN = opcode & 0x0FFF;

    if (opcode == 0x00E0) {
        clear_screen();
        return;
    }

    /* first nibble that tells you what kind of instruction it is. */
    switch (opcode & 0xF000) {
        /* 1NNN: Jump to NNN */
    case 0x1000:
        pc = NNN;
        break;
        /* 2NNN: Calls the subroutine at memory location NNN */
    case 0x2000: {
        stack_push(pc);
        pc = NNN;
        break;
    }
    /* 00EE: Returning from a subroutine */
    case 0x0000:
        pc = stack_pop();
        break;
        /* 3XNN: Will skip one instruction if the value in VX is equal to NN */
    case 0x3000:
        if (V[X] == NN) pc += 2;
        break;
        /* 4XNN: Will skip one instruction if the value in VX is not equal to NN */
    case 0x4000:
        if (!(V[X] == NN)) pc += 2;
        break;
        /* 5XY0: Skips if the values in VX and VY are equal */
    case 0x5000:
        if (V[X] == V[Y]) pc += 2;
        break;
        /* 9XY0: Skips if the values in VX and VY are not equal */
    case 0x9000:
        if (!(V[X] == V[Y])) pc += 2;
        break;
        /* Set the register VX to the value NN */
    case 0x6000:
        V[X] = NN;
        break;
        /* Add the value NN to register VX */
    case 0x7000:
        V[X] += NN;
        break;
        /* Logical and arithmetic instructions */
    case 0x8000: {
        switch (N) {
            /* 8XY0: VX is set to the value of VY */
        case 0x0000:
            V[X] = V[Y];
            break;
            /* 8XY1: VX is set to the bitwise OR of VX and VY */
        case 0x0001:
            V[X] |= V[Y];
            break;
            /* 8XY2: VX is set to the bitwise AND of VX and VY */
        case 0x0002:
            V[X] &= V[Y];
            break;
            /* 8XY3: VX is set to the bitwise XOR of VX and VY */
        case 0x0003:
            V[X] ^= V[Y];
            break;
            /* 8XY4: VX is set to the value of VX plus the value of VY */
        case 0x0004: {
            uint16_t sum = V[X] + V[Y];
            V[X] = sum & 0xFF;
            V[0xF] = (sum > 255) ? 1 : 0;
            break;
        }
        /* 8XY5: Sets VX to the result of VX - VY */
        case 0x0005: {
            uint8_t vx = V[X];
            uint8_t vy = V[Y];

            V[X] -= V[Y];
            V[0xF] = (vx >= vy);
            break;
        }
        /* 8XY7: Sets VX to the result of VY - VX */
        case 0x0007: {
            V[X] = V[Y] - V[X];
            V[0xF] = (V[Y] >= V[X]);
            break;
        }
        /* 8XY6: Put the value of VY into VX, shift VX 1 bit to the right */
        case 0x0006: {
            uint8_t value = uses_vy ? V[Y] : V[X];

            /* take the least significant bit */
            uint8_t lsb = value & 0x01;
            /* shift right */
            uint8_t result = value >> 1;

            V[X] = result;
            V[0xF] = lsb;

            break;
        }
        /* 8XYE: Put the value of VY into VX, shift VX 1 bit to the left */
        case 0x000E: {
            uint8_t value = uses_vy ? V[Y] : V[X];

            /* take the most significant bit */
            uint8_t msb = (value & 0x80) >> 7;
            /* shift left */
            uint8_t result = value << 1;

            V[X] = result;
            V[0xF] = msb;

            break;
        }
        }
        break;
    }
    /* Sets the index register I to the value NNN */
    case 0xA000:
        I = NNN;
        break;
        /* BNNN: Jump with offset */
    case 0xB000: {
        if (new_jump) {
            uint16_t XNN = ((uint16_t) X << 8) | NN;
            pc = XNN + V[X];
        } else {
            pc = NNN + V[0];
        }
        break;
    }
    /* CXNN: Generates a random number, binary ANDs it with the value NN, and puts the result in VX */
    case 0xC000: {
        uint8_t r = GetRandomValue(0, 255);
        V[X] = r & NN;
        break;
    }
    /* Display */
    case 0xD000: {
        uint8_t x = V[X] % WIDTH;
        uint8_t y = V[Y] % HEIGHT;

        V[0xF] = 0;

        for (size_t row = 0; row < N; ++row) {

            if (y + row >= HEIGHT) break;

            uint8_t sprite_byte = memory[I + row];

            for (size_t col = 0; col < 8; ++col) {

                if (x + col >= WIDTH) break;

                uint8_t bit = (sprite_byte >> (7 - col)) & 1;

                if (bit) {
                    if (display[y + row][x + col]) {
                        display[y + row][x + col] = false;
                        V[0xF] = 1;
                    } else {
                        display[y + row][x + col] = true;
                    }
                }

                ++X;
            }

            ++Y;
        }
        break;
    }
    /* Skip if key */
    case 0xE000: {
        switch (NN) {
            /* EX9E will skip one instruction if the key corresponding to the value in VX is pressed. */
        case 0x9E:
            if (keypad[V[X]]) pc += 2;
            break;
            /* EXA1 skips if the key corresponding to the value in VX is not pressed. */
        case 0xA1:
            if (!keypad[V[X]]) pc += 2;
            break;
        }
        break;
    }
    /* Timers */
    case 0xF000: {
        switch (NN) {
            /* FX07 sets VX to the current value of the delay timer */
        case 0x07:
            V[X] = delay_timer;
            break;
            /* FX15 sets the delay timer to the value in VX */
        case 0x15:
            delay_timer = V[X];
            break;
            /* FX18 sets the sound timer to the value in VX */
        case 0x18:
            sound_timer = V[X];
            break;
            /* FX1E: The index register I will get the value in VX added to it. */
        case 0x1E:
            uint16_t sum = I + V[X];
            if (sum > 0x0FFF) V[0xF] = 1;
            else V[0xF] = 0;
            I = sum;
            break;
            /* FX0A: Get key */
        case 0x0A: {
            bool waiting_for_release = false;
            bool key_handled = false;
            int pressed_key = -1;

            for (int k = 0; k < NUM_KEY; ++k) {
                if (!waiting_for_release) {
                    if (IsKeyPressed(keypad[k])) {
                        pressed_key = k;
                        waiting_for_release = true;
                    }
                } else {
                    if (k == pressed_key && IsKeyReleased(keypad[k])) {
                        V[X] = k;

                        waiting_for_release = false;
                        pressed_key = -1;

                        key_handled = true;
                        break;
                    }
                }
            }
            if (!key_handled) pc -= 2;
            break;
        }
        /* FX29: Font character */
        case 0x29:
            uint8_t digit = V[X] & 0x0F;
            I = FONTSET_START_ADDRESS + (digit * 5);
            break;
            /* FX33: Binary-coded decimal conversion */
        case 0x33: {
            int r = V[X];
            int digit;
            for (int i = 2; i >= 0; --i) {
                digit = r % 10;
                r /= 10;

                memory[I + i] = digit;
            }
            break;
        }
        /* FX55: Store memory */
        case 0x55: {
            if (modify_I) {
                for (uint8_t j = 0; j <= X; ++j) {
                    write(I, V[j]);
                    ++I;
                }
            } else {
                for (uint8_t j = 0; j <= X; ++j) {
                    write(I + j, V[j]);
                }
            }
            break;
        }
        /* FX65: load memory */
        case 0x65: {
            if (modify_I) {
                for (uint8_t j = 0; j <= X; ++j) {
                    V[j] = memory[I];
                    ++I;
                }
            } else {
                for (uint8_t j = 0; j <= X; ++j) {
                    V[j] = memory[I + j];
                }
            }
            break;
        }
        }
    }
    break;
    }
}

static void load_rom(const char * path) {
    FILE * rom = fopen(path, "rb");
    if (!rom) {
        perror("fopen");
        exit(1);
    }
    fseek(rom, 0, SEEK_END);
    long size = ftell(rom);
    if (size < 0) {
        perror("ftell");
        fclose(rom);
        exit(1);
    }
    rewind(rom);

    if ((size_t) size > MEMORY_SIZE - 0x200) {
        fprintf(stderr, "ROM too big (%ld bytes)\n", size);
        fclose(rom);
        exit(1);
    }

    fread( & memory[0x200], 1, (size_t) size, rom);
    fclose(rom);
}

int main(int argc, char * argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: ./chip8 <path to rom>\n");
        exit(1);
    }

    chip8_init();
    if (!load_fontset()) {
        perror("load_fontset");
        exit(0);
    }
    load_rom(argv[1]);

    InitWindow(WIDTH * SCALE, HEIGHT * SCALE, "CHIP-8");

    SetTargetFPS(FPS);

    float cpu_accumulator   = 0.0f;
    float timer_accumulator = 0.0f;

    while (!WindowShouldClose()) {

        const float dt = GetFrameTime();

        cpu_accumulator   += dt;
        timer_accumulator += dt;

        keypad_state();

        while (cpu_accumulator >= (1.0f / (float) CPU_HZ)) {
            decode_and_execute();
            cpu_accumulator -= (1.0f / (float) CPU_HZ);
        }

        while (timer_accumulator >= (1.0f / (float) FPS)) {
            if (delay_timer > 0) delay_timer--;
            if (sound_timer > 0) sound_timer--;
            timer_accumulator -= (1.0f / (float) FPS);
        }

        BeginDrawing();
        ClearBackground(BLACK);
        draw_screen();
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
