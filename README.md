Node.js on Chakra and Universal Windows Platform
===
This project enables Node.js to optionally use the Chakra JavaScript engine and be loaded as a module in a UWP (Universal Windows Platform) application.
This project is still **work in progress** and not an officially supported Node.js branch.


### How it works

To enable building and running Node.js with the Chakra JavaScript engine, a V8 API shim (ChakraShim) is created on top of the Chakra runtime hosting API ([JSRT]
(https://msdn.microsoft.com/en-us/library/dn249673(v=vs.94).aspx)). ChakraShim implements the most essential V8 APIs so that the underlying JavaScript engine change 
is transparent to Node.js and other native addon modules written for V8.
To enable running in a [UWP application](https://msdn.microsoft.com/en-us/windows/uwp/get-started/whats-a-uwp), code that isn't supported (i.e. doesn't pass 
[WACK (Windows App Certification Kit)](https://developer.microsoft.com/en-us/windows/develop/app-certification-kit) tests) has either been removed or replaced 
(`node_uwp_dll` in gyp files and the `UWP_DLL` macro in source files can be used to identify the changes).


### How to build

If you are looking to build this yourself. Here's what you will need:
* Windows 10
* [Python 2.6 or 2.7](https://www.python.org)
* [Visual Studio 2015 with Windows 10 SDK](https://www.visualstudio.com/en-us/downloads/download-visual-studio-vs.aspx)

Build Command for node.dll - Node.js (Chakra) for UWP
```batch
vcbuild chakra uwp-dll nosign [x86|x64|arm]
```

### How to test node.dll

See [this](https://github.com/ms-iot/node-uwp-wrapper#testing)