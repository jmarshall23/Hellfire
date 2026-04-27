# Hellfire Source Code Modernization

Experimental modernization work for the **Hellfire** source code, focused on restoring buildability on modern Windows systems and replacing legacy rendering paths with a small compatibility shim.

## Overview

This project contains work-in-progress updates around the leaked Hellfire source code that appeared on archive.org, including:

- A `ddraw` compatibility shim
- Rendering redirected through **Direct3D 11**
- Early cleanup for modern Visual Studio builds
- `scroll.asm` converted/ported to C
- x86 assembly reduction for future x64 compatibility

## Goals

- Preserve the original game behavior as closely as possible
- Replace legacy DirectDraw usage with a D3D11-backed renderer
- Remove dependency on hand-written assembly where practical
- Make the code easier to debug, maintain, and eventually port
- Keep the original software-era rendering assumptions intact

## Current Features

### DirectDraw Shim

A custom `ddraw` layer has been added so the original code can continue using its old DirectDraw-style calls while internally rendering through Direct3D 11.

The goal is not to rewrite the renderer from scratch, but to provide a compatibility layer that lets the original code path survive while targeting a modern graphics API.

### `scroll.asm` Ported to C

The original `scroll.asm` routines have been ported to C.

This helps with:

- x64 compatibility
- easier debugging
- removing legacy calling convention issues
- reducing dependency on MASM/x86-only build paths

## Status

This is an experimental source restoration and modernization project.

Expected issues:

- Rendering bugs
- Palette/lighting mismatches
- Missing or incomplete DirectDraw behavior
- Timing differences
- Unported assembly routines
- Build configuration problems

## Legal Notice

This repository is for historical preservation, research, and compatibility work only.

No original game assets are included. You must own a legitimate copy of the game to use any required data files.

The original Hellfire source code and related assets remain the property of their respective copyright holders.
