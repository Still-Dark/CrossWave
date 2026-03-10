/**
 * @file main.cpp
 * @brief Application entry point — delegates entirely to ui_interface.cpp (WinMain).
 *
 * The actual WinMain is defined in ui_interface.cpp which contains the full
 * Win32 + D3D11 + ImGui host loop.
 *
 * This file exists as the named 'main.cpp' for clarity in the project structure.
 * All initialization, threading, audio pipeline management, and UI rendering
 * live in ui_interface.cpp.
 *
 * Build note:
 *   Both this file and ui_interface.cpp are compiled. Only ui_interface.cpp
 *   defines WinMain. This file is a documentation stub.
 */

// Intentionally empty — see ui_interface.cpp for the full program entry point.
