# WESP ProcessCreate POC

A small consumer for the undocumented Windows Endpoint Security Platform
(WESP). It uses `%SystemRoot%\System32\espclient.dll` to:

- register and enumerate WESP clients;
- install a `ProcessCreate` rule (event type `1000`);
- print the PID, image path, and command line for matching events; and
- remove a registered client.

## Build

Open `WespConsumerPOC.sln` in Visual Studio 2022 and build the solution. The
project requires Desktop development with C++, MSVC v143, and a Windows SDK.
Install the ARM64 build tools when targeting ARM64.

The executable is written to:

```text
bin\<architecture>\<configuration>\wesp-consumer.exe
```

No WDK, private SDK, import library, or bundled DLL is required.

## Use

Run from an elevated terminal on a WESP-enabled system:

```powershell
# Register a client and save the GUID it prints.
.\wesp-consumer.exe register

# Enumerate registered clients.
.\wesp-consumer.exe clients

# Monitor process creation for 60 seconds.
.\wesp-consumer.exe monitor '{CLIENT-GUID}' 60

# Remove the client and its WESP objects.
.\wesp-consumer.exe remove '{CLIENT-GUID}'
```

Passing `0` as the monitoring duration runs until Ctrl+C.

## Compatibility

WESP and `espclient.dll` are undocumented. The recovered declarations and
notification decoder are version-dependent. This POC was tested on ARM64 with
`espclient.dll` version `0.1.0.154553750`:

```text
SHA-256: cb437bf5415266e2d32b96c571be3f772d81f5348a3d9f3a352707dee9287d01
```

The x64 configuration builds successfully but has not been runtime-tested.
