# Coding Style Guide

This document outlines the coding style for the `oe-heatgrip-gdo` projects.

## Variable Naming

- **Local variables**: Use `snake_case`.
- **Global variables**: Use `g_snake_case`. Reserved for variables that are
  genuinely global (external linkage).
- **Member variables**: Use `m_snake_case` — class members, and variables that
  are local to a single file. File-scope variables are **always** declared
  `static`, even inside an anonymous namespace, so the internal linkage is
  visible at the declaration.
- **Functions**: Use `lowerPascalCase` (camelCase).
- **Constants and Macros**: Use `UPPER_CASE`.
- **Classes/Structs**: Use `PascalCase` (e.g., `struct MessagePacket;`).
- **Enums**: Use `enum class` (scoped enums) with `PascalCase` members (e.g., `enum class DeviceState { ... };`).
- **Acronyms**: Keep acronyms in uppercase (e.g., `OTA`, `GDO`, `PMK`, `SSID`, `WAP`, `PWM`).
- **Pointers and references**: The `*` or `&` binds to the **type**, not the
  name: `const char* name`, `SysConfig* cfg`, `const StripConfig& sc`.
- **Brackets**: The starting brace `{` **MUST** be on a new line (Allman style).

### Type Prefixes (Hungarian Notation)

> [!IMPORTANT]
> **DO NOT** use prefixes linked to the type in variable names.
>
> - **Incorrect**: `bool b_is_active`, `uint32_t u32_count`, `char* sz_name`, `void* p_data`.
> - **Correct**: `bool is_active`, `uint32_t count`, `char* name`, `void* data`.

## Const Correctness

- **Always mark a variable `const` if it never changes after initialization.** This applies to local variables, parameters passed by reference/pointer, and member functions that don't mutate state.
  - **Correct**: `const int count = readCount();`, `const KartConfig cfg = configSnapshot();`, `void print() const;`.
  - **Incorrect**: `int count = readCount();` when `count` is never reassigned.
- Prefer `constexpr` over `const` for values known at compile time.

## Magic Numbers and Strings

- **Never use magic numbers or magic strings.** Every literal that carries
  meaning gets a named `constexpr` (preferred) or macro: timeouts, sizes,
  task priorities, queue depths, GPIO numbers, protocol field numbers,
  colors, identifiers.
  - **Incorrect**: `xQueueCreate(8, ...)`, `if (now - t > 25000)`, `g = 14;`.
  - **Correct**: `constexpr int CTRL_QUEUE_DEPTH = 8;`,
    `constexpr uint32_t BRAKE_HOLDOFF_MS = 25000;`,
    `constexpr Rgb COLOR_OK_GREEN = { 0, 14, 0 };`.
- Self-describing literals are fine: `0`, `1`, bounds derived from `sizeof`,
  and data tables whose values ARE the content (e.g. effect step definitions).

## Conditions

- **Yoda conditions**: Place the constant on the left side of comparisons to prevent accidental assignment.
  - **Correct**: `if (0 == getValue())`, `if (ESP_OK == esp_init())`.
  - **Incorrect**: `if (getValue() == 0)`, `if (esp_init() == ESP_OK)`.

## Project Structure

- Source files should use the `.cpp` extension.
- Namespaces should be used to group related functions and variables (e.g., `HardwareGPIO`, `Settings`, `WebServer`, `MemBlock`).