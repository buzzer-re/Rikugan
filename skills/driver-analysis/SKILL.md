---
name: driver-analysis
description: Windows kernel driver analysis — DriverEntry, dispatch table, IOCTL handlers, vulnerability audit
---
Task: Windows Kernel Driver Analysis. You are analyzing a kernel-mode driver binary.

## Mandatory First Steps

1. Find DriverEntry — usually the binary entry point
   - Signature: `NTSTATUS DriverEntry(DRIVER_OBJECT*, UNICODE_STRING*)`
   - Use `decompile_function` on the entry point
2. From DriverEntry, extract:
   - MajorFunction dispatch table assignments
   - DriverUnload pointer
   - DeviceName and SymbolicLinkName
3. Identify IOCTL handlers — look for IRP_MJ_DEVICE_CONTROL dispatch entry

## Key Data Structures

Use `create_struct` and `set_type` early — these appear in virtually every driver:
- DRIVER_OBJECT, DEVICE_OBJECT
- IRP, IO_STACK_LOCATION
- UNICODE_STRING

Apply types with `set_function_prototype` and `apply_type_to_variable` to make decompiled code readable immediately.

## IOCTL Analysis

For each IRP_MJ_DEVICE_CONTROL handler:
1. `decompile_function` on the dispatch function
2. Find the switch statement on IoControlCode
3. For each IOCTL code, document:
   - IOCTL value and decoded method/access
   - Expected input/output buffer sizes
   - Operation performed
4. Check for dangerous patterns:
   - Kernel memory read/write gadgets
   - Process token manipulation
   - Arbitrary code execution paths

## Common Vulnerabilities to Flag

- **KeSetEvent with user-controlled address** — kernel write primitive
- **Missing ProbeForRead/ProbeForWrite** before kernel-mode buffer copy
- **Unchecked buffer sizes in METHOD_NEITHER IOCTLs** — pool overflow
- **MmMapIoSpace with user-supplied physical address** — arbitrary physical memory access
- **Direct stack buffer reads without size validation** — kernel stack overflow
- **ObReferenceObjectByHandle without proper access checks**

## Analysis Workflow

1. Map the dispatch table → understand all supported IRPs
2. Deep-dive each IOCTL handler → document input/output
3. Trace data flow from usermode input to kernel operations
4. Flag every path where user-controlled data reaches a sensitive kernel API
5. Rename functions as you understand them: `DispatchDeviceControl`, `HandleIoctlReadPhysMem`, etc.
